#include "pomodoro/taskbar_status_panel.h"
#include "pomodoro/taskbar_layout.h"
#include "pomodoro/taskbar_obstacles.h"

#ifdef _WIN32
#include <shellapi.h>
#include <algorithm>
#include <cstring>
#include <utility>

namespace pomodoro {
namespace {
constexpr wchar_t panelClass[] = L"PomodoroNative.TaskbarStatus.v1";
constexpr UINT_PTR layoutTimerId = 1;

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

bool TaskbarStatusPanel::locate(HWND& host, RECT& hostBounds) {
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
    if (window_) {
        ShowWindow(window_, SW_HIDE);
        if (layoutTimer_) KillTimer(window_, layoutTimerId);
    }
    layoutTimer_ = false;
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
    lastLayoutCheck_ = GetTickCount64();
    struct Reset { bool& value; ~Reset() { value = false; } } reset{positioning_};
    HWND host = nullptr;
    RECT taskbar{};
    if (!locate(host, taskbar)) return;
    DpiScope dpiContext(host);
    if (window_ && (!IsWindow(window_) || host_ != host)) {
        if (IsWindow(window_)) DestroyWindow(window_);
        window_ = nullptr;
    }
    if (!window_ && !create(host)) return;
    if (!layoutTimer_) {
        layoutTimer_ = SetTimer(window_, layoutTimerId, 1000, nullptr) != 0;
        if (!layoutTimer_) { fail(L"Windows could not monitor available taskbar space."); return; }
    }

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
    const TaskbarRect strip{MulDiv(48, static_cast<int>(dpi), 96), inset,
        static_cast<int>(taskbar.right) - inset, static_cast<int>(taskbar.bottom) - inset};
    const auto snapshot = collectTaskbarObstacles(host, window_, !explicitHost_);
    if (!snapshot.taskButtonsKnown) {
        fail(L"Waiting for taskbar button bounds before showing timer status.");
        return;
    }
    const int preferredRight = snapshot.widgetLeft ? *snapshot.widgetLeft - gap : strip.right;
    const auto slot = findTaskbarSlot(strip, preferredRight, width, height, gap, snapshot.occupied);
    if (!slot) {
        fail(L"There is no free taskbar space for timer status. It will return when space is available.");
        return;
    }
    RECT bounds{slot->left, slot->top, slot->right, slot->bottom};
    RECT previous{};
    if (!GetWindowRect(window_, &previous)) { fail(L"Windows could not read the timer panel position."); return; }
    MapWindowPoints(HWND_DESKTOP, host, reinterpret_cast<POINT*>(&previous), 2);
    // Keep a moving panel hidden until its actual, potentially DPI-adjusted
    // rectangle has been checked. Never paint a guessed position first.
    if (!EqualRect(&previous, &bounds)) ShowWindow(window_, SW_HIDE);
    if (!SetWindowPos(window_, HWND_TOP, bounds.left, bounds.top, width, height,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER)) {
        fail(L"Windows could not position the timer taskbar status panel.");
        return;
    }
    RECT actual{};
    if (!GetWindowRect(window_, &actual)) { fail(L"Windows could not verify the timer panel position."); return; }
    MapWindowPoints(HWND_DESKTOP, host, reinterpret_cast<POINT*>(&actual), 2);
    const auto latest = collectTaskbarObstacles(host, window_, !explicitHost_);
    const TaskbarRect placed{actual.left, actual.top, actual.right, actual.bottom};
    if (!latest.taskButtonsKnown || actual.right - actual.left != width || actual.bottom - actual.top != height ||
        !taskbarSlotIsFree(placed, strip, gap, latest.occupied)) {
        fail(L"Taskbar layout changed; timer status is hidden until a safe gap is available.");
        return;
    }
    updateColors(host, actual);
    ShowWindow(window_, SW_SHOWNOACTIVATE);
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
    case taskbarObstaclesReadyMessage: {
        const bool available = self->layoutAvailable_;
        const auto error = self->error_;
        self->reposition();
        if (available != self->layoutAvailable_ || error != self->error_)
            PostMessageW(self->owner_, taskbarLayoutChangedMessage, 0, 0);
        return 0;
    }
    case WM_TIMER:
        if (wParam == layoutTimerId && GetTickCount64() - self->lastLayoutCheck_ >= 900) {
            const bool available = self->layoutAvailable_;
            const auto error = self->error_;
            self->reposition();
            if (available != self->layoutAvailable_ || error != self->error_)
                PostMessageW(self->owner_, taskbarLayoutChangedMessage, 0, 0);
        }
        return 0;
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
        self->layoutTimer_ = false;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        break;
    default: break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace pomodoro
#endif
