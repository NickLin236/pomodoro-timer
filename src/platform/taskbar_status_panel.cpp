#include "pomodoro/taskbar_status_panel.h"

#ifdef _WIN32
#include <shellapi.h>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <map>
#include <vector>
#include <utility>

namespace pomodoro {
namespace {
constexpr wchar_t panelClass[] = L"PomodoroNative.TaskbarStatus.v1";
constexpr wchar_t trafficMonitorTitle[] = L"TrafficMonitorTaskbarWindow";

template <typename Function>
Function user32Function(const char* name) {
    const auto address = GetProcAddress(GetModuleHandleW(L"user32.dll"), name);
    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(address));
    std::memcpy(&function, &address, sizeof(function));
    return function;
}

// Creating the child with Explorer's DPI context avoids cross-process
// parent/child awareness mismatches. The main UI thread's context is restored.
class DpiScope final {
public:
    explicit DpiScope(HWND host) {
        // DPI_AWARENESS_CONTEXT is an opaque pointer-sized handle. HANDLE
        // keeps these dynamically loaded calls buildable with older SDKs.
        using GetContext = HANDLE (WINAPI*)(HWND);
        const auto get = user32Function<GetContext>("GetWindowDpiAwarenessContext");
        set_ = user32Function<SetContext>("SetThreadDpiAwarenessContext");
        if (get && set_) previous_ = set_(get(host));
    }
    ~DpiScope() { if (set_ && previous_) set_(previous_); }
private:
    using SetContext = HANDLE (WINAPI*)(HANDLE);
    SetContext set_ = nullptr;
    HANDLE previous_ = nullptr;
};

bool intersects(const RECT& first, const RECT& second) {
    RECT intersection{};
    return IntersectRect(&intersection, &first, &second) != FALSE;
}

struct AnchorSearch {
    HWND host;
    RECT clientBounds;
    DWORD hostProcess = 0;
    bool found = false;
    RECT boundary{};
    std::map<DWORD, bool> trafficProcesses{};
    unsigned textRequests = 0;
};

bool isContainer(const std::wstring& name) {
    return name == L"ReBarWindow32" || name == L"WorkerW" ||
        name == L"MSTaskSwWClass" || name == L"MSTaskListWClass" ||
        name == L"Windows.UI.Composition.DesktopWindowContentBridge";
}

bool isTaskListDescendant(HWND window, HWND host) {
    for (HWND parent = window; parent && parent != host; parent = GetParent(parent)) {
        wchar_t name[128]{};
        GetClassNameW(parent, name, static_cast<int>(std::size(name)));
        if (std::wstring(name) == L"MSTaskSwWClass" || std::wstring(name) == L"MSTaskListWClass")
            return true;
    }
    return false;
}

bool isTrafficMonitorProcess(DWORD process, AnchorSearch& search) {
    if (!process || process == search.hostProcess || process == GetCurrentProcessId()) return false;
    const auto cached = search.trafficProcesses.find(process);
    if (cached != search.trafficProcesses.end()) return cached->second;
    bool trafficMonitor = false;
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process);
    if (handle) {
        std::vector<wchar_t> path(32768);
        DWORD length = static_cast<DWORD>(path.size());
        if (QueryFullProcessImageNameW(handle, 0, path.data(), &length)) {
            const std::wstring image(path.data(), length);
            const auto slash = image.find_last_of(L"\\/");
            const std::wstring name = image.substr(slash == std::wstring::npos ? 0 : slash + 1);
            trafficMonitor = name.size() >= 18 &&
                CompareStringOrdinal(name.c_str(), 14, L"TrafficMonitor", 14, TRUE) == CSTR_EQUAL &&
                CompareStringOrdinal(name.c_str() + name.size() - 4, 4, L".exe", 4, TRUE) == CSTR_EQUAL;
        }
        CloseHandle(handle);
    }
    search.trafficProcesses.emplace(process, trafficMonitor);
    return trafficMonitor;
}

void includeBoundary(AnchorSearch& search, const RECT& bounds) {
    if (!search.found || bounds.left < search.boundary.left) search.boundary = bounds;
    search.found = true;
}

BOOL examineTaskbarWindow(HWND window, AnchorSearch& search, bool descendant) {
    if (descendant) {
        // Explorer can retain hidden children while rebuilding its taskbar.
        for (HWND current = window; current && current != search.host; current = GetParent(current)) {
            if (!(GetWindowLongPtrW(current, GWL_STYLE) & WS_VISIBLE)) return TRUE;
        }
    } else if (!(GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE)) return TRUE;
    RECT bounds{};
    if (!GetWindowRect(window, &bounds)) return TRUE;
    MapWindowPoints(HWND_DESKTOP, search.host, reinterpret_cast<POINT*>(&bounds), 2);
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top ||
        !intersects(search.clientBounds, bounds)) return TRUE;
    wchar_t classBuffer[128]{};
    GetClassNameW(window, classBuffer, static_cast<int>(std::size(classBuffer)));
    const std::wstring className(classBuffer);
    if (className == panelClass || className == L"tooltips_class32" || isContainer(className)) return TRUE;

    const LONG hostWidth = search.clientBounds.right - search.clientBounds.left;
    const LONG hostHeight = search.clientBounds.bottom - search.clientBounds.top;
    const bool compact = bounds.right - bounds.left <= hostWidth / 2 &&
        bounds.bottom - bounds.top <= hostHeight * 2 &&
        bounds.top >= search.clientBounds.top - 2 && bounds.bottom <= search.clientBounds.bottom + 2;
    if (!compact && className != L"TrayNotifyWnd") return TRUE;

    wchar_t title[128]{};
    GetWindowTextW(window, title, static_cast<int>(std::size(title)));
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    bool trafficMonitor = std::wstring(title) == trafficMonitorTitle || className == trafficMonitorTitle;
    if (!trafficMonitor && compact && process != search.hostProcess && process != GetCurrentProcessId()) {
        // GetWindowText does not reliably retrieve custom text from another
        // process's child controls. A bounded request also handles MFC dialogs
        // whose caption cache is empty. Never let a hung widget block a tick.
        DWORD_PTR copied = 0;
        if (search.textRequests++ < 3 && SendMessageTimeoutW(window, WM_GETTEXT, std::size(title), reinterpret_cast<LPARAM>(title),
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &copied))
            trafficMonitor = std::wstring(title) == trafficMonitorTitle;
        if (!trafficMonitor) trafficMonitor = isTrafficMonitorProcess(process, search);
    }
    if (trafficMonitor) includeBoundary(search, bounds);
    // News and interests, taskbar toolbars and other compact widgets can sit
    // between Traffic Monitor and the tray, or to its left. Use the left edge
    // of the whole right-side group, not just the tray's immediate neighbour.
    // Task-list containers span unused button space, so they are not widgets.
    if (descendant && compact && bounds.left >= search.clientBounds.left + hostWidth / 2 &&
        !isTaskListDescendant(window, search.host))
        includeBoundary(search, bounds);
    if (className == L"TrayNotifyWnd") includeBoundary(search, bounds);
    return TRUE;
}

BOOL CALLBACK findTaskbarAnchor(HWND window, LPARAM result) {
    return examineTaskbarWindow(window, *reinterpret_cast<AnchorSearch*>(result), true);
}
BOOL CALLBACK findFloatingTrafficMonitor(HWND window, LPARAM result) {
    return examineTaskbarWindow(window, *reinterpret_cast<AnchorSearch*>(result), false);
}
} // namespace

TaskbarStatusPanel::TaskbarStatusPanel(HINSTANCE instance, HWND owner,
                                     UINT callbackMessage, HWND taskbarHost)
    : instance_(instance), owner_(owner), callbackMessage_(callbackMessage),
      explicitHost_(taskbarHost) {}

TaskbarStatusPanel::~TaskbarStatusPanel() {
    requestedVisible_ = false;
    if (window_ && IsWindow(window_)) DestroyWindow(window_);
    if (font_) DeleteObject(font_);
}

bool TaskbarStatusPanel::create(HWND host) {
    WNDCLASSEXW type{};
    type.cbSize = sizeof(type);
    type.lpfnWndProc = windowProcedure;
    type.hInstance = instance_;
    type.hCursor = LoadCursorW(nullptr, IDC_HAND);
    type.lpszClassName = panelClass;
    WNDCLASSEXW existing{};
    existing.cbSize = sizeof(existing);
    if (!GetClassInfoExW(instance_, panelClass, &existing) &&
        !RegisterClassExW(&type)) {
        fail(L"Windows could not register the taskbar status panel.");
        return false;
    }
    DpiScope dpiContext(host);
    const std::wstring caption = firstLine_ + L"\n" + secondLine_;
    window_ = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_NOPARENTNOTIFY,
        panelClass, caption.c_str(), WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1, host,
        nullptr, instance_, this);
    if (!window_) {
        fail(L"Windows could not attach the status panel to the taskbar.");
        return false;
    }
    host_ = host;
    return true;
}

bool TaskbarStatusPanel::locate(HWND& host, RECT& hostBounds, RECT& anchorBounds) {
    host = explicitHost_ ? explicitHost_ : FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!host || !IsWindow(host)) {
        fail(L"The Windows taskbar is unavailable; status will return when Explorer is ready.");
        return false;
    }
    DpiScope dpiContext(host);
    if (!GetClientRect(host, &hostBounds)) {
        fail(L"Windows could not read the taskbar's available display area.");
        return false;
    }
    if (hostBounds.right - hostBounds.left <= hostBounds.bottom - hostBounds.top) {
        fail(L"The status panel requires a horizontal Windows taskbar.");
        return false;
    }
    // The task-list class and nesting differ between Explorer releases and
    // taskbar replacements. Placement needs only a visible anchor, so do not
    // reject a working Traffic Monitor based on unrelated task-list classes.
    AnchorSearch search{host, hostBounds};
    GetWindowThreadProcessId(host, &search.hostProcess);
    EnumChildWindows(host, findTaskbarAnchor, reinterpret_cast<LPARAM>(&search));
    // Traffic Monitor may fall back to a tiny top-level taskbar-aligned window
    // when SetParent fails. Its source explicitly supports that configuration.
    if (!explicitHost_) EnumWindows(findFloatingTrafficMonitor, reinterpret_cast<LPARAM>(&search));
    if (!search.found) {
        fail(L"Could not find Traffic Monitor or the taskbar notification area for the status panel.");
        return false;
    }
    anchorBounds = search.boundary;
    return true;
}

void TaskbarStatusPanel::update(std::wstring firstLine, std::wstring secondLine,
                              bool visible) {
    const bool textChanged = firstLine_ != firstLine || secondLine_ != secondLine;
    firstLine_ = std::move(firstLine);
    secondLine_ = std::move(secondLine);
    requestedVisible_ = visible;
    if (!visible) { hide(); return; }
    reposition();
    if (window_) {
        if (textChanged) {
            SetWindowTextW(window_, (firstLine_ + L"\n" + secondLine_).c_str());
            InvalidateRect(window_, nullptr, FALSE);
        }
    }
}

void TaskbarStatusPanel::hide() {
    requestedVisible_ = false;
    if (window_) ShowWindow(window_, SW_HIDE);
}

void TaskbarStatusPanel::explorerRestarted() {
    if (window_ && IsWindow(window_)) DestroyWindow(window_);
    window_ = nullptr;
    host_ = nullptr;
    layoutAvailable_ = false;
    if (requestedVisible_) reposition();
}

void TaskbarStatusPanel::updateFont(UINT dpi, int height) {
    const int pixelHeight = (std::max)(8, (std::min)(MulDiv(9, static_cast<int>(dpi), 72), (height - 2) / 2));
    if (font_ && dpi_ == dpi && fontHeight_ == pixelHeight) return;
    if (font_) DeleteObject(font_);
    font_ = CreateFontW(-pixelHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    dpi_ = dpi;
    fontHeight_ = pixelHeight;
}

void TaskbarStatusPanel::updateColors(HWND host, const RECT& bounds) {
    DWORD lightTheme = 0;
    DWORD bytes = sizeof(lightTheme);
    RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &lightTheme, &bytes);
    COLORREF background = lightTheme ? RGB(235, 235, 235) : RGB(32, 32, 32);
    HDC dc = GetDC(host);
    if (dc) {
        // Sample the taskbar's top edge outside our inset child. This follows
        // opaque custom taskbar colors without sampling our previous paint.
        const auto sampled = GetPixel(dc, (bounds.left + bounds.right) / 2, 0);
        if (sampled != CLR_INVALID) background = sampled;
        ReleaseDC(host, dc);
    }
    const unsigned luminance = 299 * GetRValue(background) + 587 * GetGValue(background) + 114 * GetBValue(background);
    const COLORREF foreground = luminance >= 145000 ? RGB(20, 20, 20) : RGB(245, 245, 245);
    if (background_ != background || foreground_ != foreground) {
        background_ = background;
        foreground_ = foreground;
        if (window_) InvalidateRect(window_, nullptr, FALSE);
    }
}

void TaskbarStatusPanel::reposition() {
    if (!requestedVisible_ || positioning_) return;
    positioning_ = true;
    struct Reset { bool& value; ~Reset() { value = false; } } reset{positioning_};
    HWND host = nullptr;
    RECT taskbar{}, anchor{};
    if (!locate(host, taskbar, anchor)) return;
    DpiScope dpiContext(host);
    if (window_ && (!IsWindow(window_) || host_ != host)) {
        if (IsWindow(window_)) DestroyWindow(window_);
        window_ = nullptr;
    }
    if (!window_ && !create(host)) return;

    using GetDpi = UINT (WINAPI*)(HWND);
    const auto getDpi = user32Function<GetDpi>("GetDpiForWindow");
    const UINT dpi = getDpi ? getDpi(window_) : 96;
    const int inset = (std::max)(1, MulDiv(2, static_cast<int>(dpi), 96));
    const int gap = (std::max)(2, MulDiv(4, static_cast<int>(dpi), 96));
    const int height = static_cast<int>(taskbar.bottom - taskbar.top) - 2 * inset;
    if (height < 18) { fail(L"The taskbar is too short to show a readable Pomodoro status panel."); return; }
    updateFont(dpi, height);
    int textWidth = 0;
    HDC dc = GetDC(window_);
    if (dc) {
        const auto previous = SelectObject(dc, font_ ? static_cast<HGDIOBJ>(font_) : GetStockObject(DEFAULT_GUI_FONT));
        SIZE first{}, second{};
        GetTextExtentPoint32W(dc, firstLine_.c_str(), static_cast<int>(firstLine_.size()), &first);
        GetTextExtentPoint32W(dc, secondLine_.c_str(), static_cast<int>(secondLine_.size()), &second);
        textWidth = (std::max)(first.cx, second.cx);
        SelectObject(dc, previous);
        ReleaseDC(window_, dc);
    }
    const int width = std::clamp(textWidth + 2 * gap, MulDiv(118, static_cast<int>(dpi), 96), MulDiv(180, static_cast<int>(dpi), 96));
    RECT bounds{anchor.left - gap - width, inset,
                anchor.left - gap, inset + height};
    if (bounds.left < MulDiv(48, static_cast<int>(dpi), 96) || bounds.right > taskbar.right - taskbar.left) {
        fail(L"There is not enough taskbar space to the left of Traffic Monitor for Pomodoro status.");
        return;
    }
    updateColors(host, bounds);
    if (!SetWindowPos(window_, HWND_TOP, bounds.left, bounds.top, width, height,
                      SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW)) {
        fail(L"Windows could not position the Pomodoro taskbar status panel.");
        return;
    }
    // Keep the child's requested visibility intact. The taskbar parent handles
    // hide/show, offscreen auto-hide movement, and fullscreen z-order; manually
    // hiding here could leave a paused panel hidden after the parent returns.
    layoutAvailable_ = true;
    error_.clear();
}

void TaskbarStatusPanel::fail(const wchar_t* error) {
    layoutAvailable_ = false;
    error_ = error;
    if (window_) ShowWindow(window_, SW_HIDE);
}

void TaskbarStatusPanel::paint() {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window_, &paint);
    RECT bounds{};
    GetClientRect(window_, &bounds);
    HBRUSH brush = CreateSolidBrush(background_);
    FillRect(dc, &bounds, brush ? brush : GetSysColorBrush(COLOR_WINDOW));
    if (brush) DeleteObject(brush);
    const auto previous = SelectObject(dc, font_ ? static_cast<HGDIOBJ>(font_) : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, foreground_);
    const int margin = (std::max)(2, MulDiv(4, static_cast<int>(dpi_ ? dpi_ : 96), 96));
    bounds.left += margin;
    bounds.right -= margin;
    RECT first = bounds, second = bounds;
    first.bottom = bounds.top + (bounds.bottom - bounds.top) / 2;
    second.top = first.bottom;
    constexpr UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;
    DrawTextW(dc, firstLine_.c_str(), static_cast<int>(firstLine_.size()), &first, format);
    DrawTextW(dc, secondLine_.c_str(), static_cast<int>(secondLine_.size()), &second, format);
    SelectObject(dc, previous);
    EndPaint(window_, &paint);
}

LRESULT CALLBACK TaskbarStatusPanel::windowProcedure(HWND window, UINT message,
                                                    WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<TaskbarStatusPanel*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<TaskbarStatusPanel*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_PAINT: self->paint(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_LBUTTONUP:
        PostMessageW(self->owner_, self->callbackMessage_, 0, MAKELPARAM(NIN_SELECT, 0)); return 0;
    case WM_CONTEXTMENU:
        PostMessageW(self->owner_, self->callbackMessage_, 0, MAKELPARAM(WM_CONTEXTMENU, 0)); return 0;
    case WM_NCDESTROY:
        self->window_ = nullptr;
        self->host_ = nullptr;
        self->layoutAvailable_ = false;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    default: break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace pomodoro
#endif
