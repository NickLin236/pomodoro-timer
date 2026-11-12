#include "pomodoro/controller.h"
#include "pomodoro/windows_shell.h"
#include "pomodoro/taskbar_status_panel.h"
#include "pomodoro/notifications.h"
#include "pomodoro/windows_notifications.h"

#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace pomodoro;
constexpr wchar_t windowClass[] = L"PomodoroNative.MainWindow.v1";
constexpr UINT trayMessage = WM_APP + 1;
constexpr UINT activateMessage = WM_APP + 2;
constexpr UINT revealFocusMessage = WM_APP + 3;
constexpr UINT notificationFailureMessage = WM_APP + 4;
constexpr ULONG_PTR notificationCopyData = 0x504F4D4E;
constexpr UINT_PTR schedulerId = 1;
constexpr DWORD mainWindowStyle = WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_HSCROLL | WS_CLIPCHILDREN;
constexpr int contentWidth = 672;
constexpr int contentHeight = 640;
enum Command : int {
    Tabs = 100, FocusInput, BreakInput, CyclesInput, PomodoroAction,
    PomodoroSkip, PomodoroReset, SavePomodoro, HoursInput, MinutesInput,
    SecondsInput, CountdownAction, CountdownReset, SaveCountdown,
    SoundCheck, StartupCheck, BackgroundCombo, TaskbarCombo, SaveSettings,
    TestNotification, OpenWindow, OpenSettings, ExitApplication,
    TogglePomodoro, ToggleCountdown, StatusCheck
};

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::wstring duration(Millis ms) {
    const Millis seconds = (std::max<Millis>)(0, ms) / 1000 + (ms > 0 && ms % 1000 != 0 ? 1 : 0);
    const auto two = [](Millis n) { return (n < 10 ? L"0" : L"") + std::to_wstring(n); };
    if (seconds >= 3600) return two(seconds / 3600) + L":" + two(seconds / 60 % 60) + L":" + two(seconds % 60);
    return two(seconds / 60) + L":" + two(seconds % 60);
}

const wchar_t* statusName(Status status) {
    switch (status) {
    case Status::Idle: return L"Ready";
    case Status::Running: return L"Running";
    case Status::Paused: return L"Paused";
    case Status::Completed: return L"Completed";
    }
    return L"";
}
bool active(Status status) { return status == Status::Running || status == Status::Paused; }

template <typename Function> Function user32Function(const char* name) {
    const auto address = GetProcAddress(GetModuleHandleW(L"user32.dll"), name);
    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(address));
    std::memcpy(&function, &address, sizeof(function));
    return function;
}

std::filesystem::path dataDirectory() {
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (!length) throw std::runtime_error("LOCALAPPDATA is unavailable; timer state cannot be stored.");
    std::wstring path(length, L'\0');
    const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", path.data(), length);
    if (!written || written >= length) throw std::runtime_error("Could not read LOCALAPPDATA.");
    path.resize(written);
    return std::filesystem::path(path) / L"PomodoroNative";
}

struct Placement { HWND control; int page, x, y, width, height; bool large; };

class MainWindow {
public:
    MainWindow(HINSTANCE instance, StateStore& store, Clock& clock)
        : instance_(instance), controller_(store, clock) {}
    ~MainWindow() {
        taskbarPanel_.reset();
        shell_.reset();
        if (font_) DeleteObject(font_);
        if (largeFont_) DeleteObject(largeFont_);
    }

    bool create() {
        WNDCLASSEXW type{};
        type.cbSize = sizeof(type);
        type.lpfnWndProc = windowProcedure;
        type.hInstance = instance_;
        type.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(101));
        type.hIconSm = type.hIcon;
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        type.lpszClassName = windowClass;
        if (!RegisterClassExW(&type)) return false;
        window_ = CreateWindowExW(WS_EX_CONTROLPARENT, windowClass, L"Pomodoro",
            mainWindowStyle, CW_USEDEFAULT, CW_USEDEFAULT, 700, 650,
            nullptr, nullptr, instance_, this);
        return window_ != nullptr;
    }

    void begin(bool background, const std::optional<NotificationAction>& activation = std::nullopt) {
        runtimeIntegration_ = true;
        auto recovered = controller_.load();
        loadInputs();
        startupActual_ = isStartAtSignInEnabled();
        Button_SetCheck(startup_, startupActual_ ? BST_CHECKED : BST_UNCHECKED);
        if (!shell_->installTray()) notice_ = L"Windows could not create the tray icon. The window will stay accessible on the taskbar.";
        shell_->initializeTaskbar();
        // Refresh the notification identity when this portable executable has
        // moved, even if no timer reaches its deadline during this launch.
        notifications_.setFailureTarget(window_, notificationFailureMessage);
        notificationsUsed_ = true;
        if (!notifications_.clear(L"test")) notificationError_ = notifications_.lastError();
        taskbarPanel_ = std::make_unique<TaskbarStatusPanel>(instance_, window_, trayMessage);
        if (activation && activation->command == NotificationCommand::Stop &&
                matchesNotificationSession(controller_.engine(), *activation)) {
            applyNotificationAction(*activation);
            recovered.events.erase(std::remove_if(recovered.events.begin(), recovered.events.end(),
                [&activation](const CompletionEvent& e) { return e.timer == activation->timer; }), recovered.events.end());
        }
        showUpdate(recovered);
        if (background && controller_.warning().empty()) backgroundWindow();
        else open();
        if (activation && activation->command == NotificationCommand::Open) applyNotificationAction(*activation);
        refresh();
    }

    HWND handle() const { return window_; }

private:
    HWND add(const wchar_t* type, const wchar_t* text, DWORD style, int id,
             int page, int x, int y, int width, int height, bool large = false) {
        HWND control = CreateWindowExW(type == std::wstring(L"EDIT") ? WS_EX_CLIENTEDGE : 0,
            type, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        if (!control) throw std::runtime_error("Could not create a native control.");
        placements_.push_back({control, page, x, y, width, height, large});
        if (!SetWindowSubclass(control, controlProcedure, 1, reinterpret_cast<DWORD_PTR>(this)))
            throw std::runtime_error("Could not initialize keyboard navigation.");
        return control;
    }
    HWND label(const wchar_t* text, int page, int x, int y, int w, int h = 24, bool large = false) {
        return add(L"STATIC", text, SS_LEFT, 0, page, x, y, w, h, large);
    }
    HWND edit(int id, int page, int x, int y, int width) {
        HWND result = add(L"EDIT", L"", WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
                          id, page, x, y, width, 28);
        SendMessageW(result, EM_SETLIMITTEXT, 8, 0);
        return result;
    }
    HWND button(const wchar_t* text, int id, int page, int x, int y, int w = 120) {
        return add(L"BUTTON", text, WS_TABSTOP | BS_PUSHBUTTON, id, page, x, y, w, 32);
    }
    void buildControls() {
        shell_ = std::make_unique<ShellIntegration>(instance_, window_, trayMessage);
        tabs_ = add(WC_TABCONTROLW, L"", WS_TABSTOP | WS_CLIPSIBLINGS, Tabs, -1, 16, 16, 640, 32);
        for (auto title : {L"Pomodoro", L"Timer", L"Settings"}) {
            TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = const_cast<wchar_t*>(title);
            TabCtrl_InsertItem(tabs_, TabCtrl_GetItemCount(tabs_), &item);
        }
        label(L"Focus (minutes)", 0, 28, 76, 155);
        label(L"Break (minutes)", 0, 222, 76, 155);
        label(L"Cycles", 0, 416, 76, 130);
        focus_ = edit(FocusInput, 0, 28, 104, 155);
        break_ = edit(BreakInput, 0, 222, 104, 155);
        cycles_ = edit(CyclesInput, 0, 416, 104, 155);
        label(L"Durations: 1–10,080 minutes. Cycles: 1–1,000.", 0, 28, 142, 580);
        button(L"Save setup", SavePomodoro, 0, 28, 177);
        pomodoroTime_ = label(L"25:00", 0, 28, 226, 590, 90, true);
        pomodoroPhase_ = label(L"Focus — cycle 1 / 4", 0, 28, 322, 580, 30);
        pomodoroCaptured_ = label(L"", 0, 28, 357, 590, 45);
        pomodoroAction_ = button(L"Start", PomodoroAction, 0, 28, 420);
        skip_ = button(L"Skip phase", PomodoroSkip, 0, 170, 420);
        button(L"Reset", PomodoroReset, 0, 312, 420);

        label(L"Hours", 1, 28, 76, 130);
        label(L"Minutes", 1, 222, 76, 130);
        label(L"Seconds", 1, 416, 76, 130);
        hours_ = edit(HoursInput, 1, 28, 104, 155);
        minutes_ = edit(MinutesInput, 1, 222, 104, 155);
        seconds_ = edit(SecondsInput, 1, 416, 104, 155);
        label(L"Enter a positive duration up to 168 hours (7 days).", 1, 28, 142, 590);
        button(L"Save duration", SaveCountdown, 1, 28, 177, 140);
        countdownTime_ = label(L"05:00", 1, 28, 226, 590, 90, true);
        countdownStatus_ = label(L"Ready", 1, 28, 322, 590, 30);
        label(L"The timer and Pomodoro can run at the same time.", 1, 28, 363, 590, 40);
        countdownAction_ = button(L"Start", CountdownAction, 1, 28, 420);
        button(L"Reset", CountdownReset, 1, 170, 420);

        sound_ = add(L"BUTTON", L"Play notification sound", WS_TABSTOP | BS_AUTOCHECKBOX,
                     SoundCheck, 2, 28, 76, 580, 30);
        startup_ = add(L"BUTTON", L"Start when I sign in (launch in background)", WS_TABSTOP | BS_AUTOCHECKBOX,
                       StartupCheck, 2, 28, 118, 590, 30);
        label(L"Install in a stable folder before enabling sign-in startup.", 2, 48, 153, 550, 40);
        statusCheck_ = add(L"BUTTON", L"Show timer status in taskbar", WS_TABSTOP | BS_AUTOCHECKBOX,
                           StatusCheck, 2, 28, 208, 590, 30);
        label(L"Shows the active timer or Pomodoro phase and remaining time.", 2, 48, 243, 550, 40);
        label(L"Preferred taskbar timer when both are active", 2, 28, 288, 570);
        source_ = add(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST,
                      TaskbarCombo, 2, 28, 318, 530, 130);
        ComboBox_AddString(source_, L"Pomodoro"); ComboBox_AddString(source_, L"General timer");
        button(L"Save settings", SaveSettings, 2, 28, 385, 140);
        button(L"Test notification", TestNotification, 2, 190, 385, 180);
        label(L"Minimize hides the app in the notification area. Close exits.\r\nNotifications depend on your Windows notification settings.",
              2, 28, 436, 590, 58);
        status_ = label(L"Ready. Minimize hides the app; Close and Exit save and quit.", -1, 28, 514, 590, 65);
        button(L"Exit", ExitApplication, -1, 524, 590, 100);
        using GetDpiForWindowFunction = UINT (WINAPI*)(HWND);
        const auto getDpi = user32Function<GetDpiForWindowFunction>("GetDpiForWindow");
        setDpi(getDpi ? getDpi(window_) : 96);
        RECT bounds{0, 0, scale(692), scale(contentHeight)};
        using AdjustForDpiFunction = BOOL (WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
        const auto adjust = user32Function<AdjustForDpiFunction>("AdjustWindowRectExForDpi");
        if (adjust) adjust(&bounds, mainWindowStyle, FALSE, WS_EX_CONTROLPARENT, dpi_);
        else AdjustWindowRectEx(&bounds, mainWindowStyle, FALSE, WS_EX_CONTROLPARENT);
        RECT current{}; GetWindowRect(window_, &current);
        current.right = current.left + bounds.right - bounds.left;
        current.bottom = current.top + bounds.bottom - bounds.top;
        fitToWorkArea(current);
        SetWindowPos(window_, nullptr, current.left, current.top,
                     current.right - current.left, current.bottom - current.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        updateScrollBars();
    }

    void setDpi(UINT dpi) {
        scrollX_ = MulDiv(scrollX_, static_cast<int>(dpi), static_cast<int>(dpi_));
        scrollY_ = MulDiv(scrollY_, static_cast<int>(dpi), static_cast<int>(dpi_));
        dpi_ = dpi;
        if (font_) DeleteObject(font_);
        if (largeFont_) DeleteObject(largeFont_);
        font_ = CreateFontW(-MulDiv(11, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH, L"Segoe UI");
        largeFont_ = CreateFontW(-MulDiv(46, static_cast<int>(dpi), 72), 0, 0, 0, FW_LIGHT, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH, L"Segoe UI");
        for (const auto& item : placements_) {
            SendMessageW(item.control, WM_SETFONT, reinterpret_cast<WPARAM>(item.large ? largeFont_ : font_), TRUE);
        }
        updateScrollBars();
        choosePage(page_);
    }

    void choosePage(int page) {
        const bool changed = page_ != page;
        page_ = page;
        for (const auto& item : placements_)
            ShowWindow(item.control, item.page == -1 || item.page == page_ ? SW_SHOW : SW_HIDE);
        if (changed) {
            scrollTo(0, 0);
            if (GetFocus() && !IsWindowVisible(GetFocus())) SetFocus(tabs_);
        }
    }

    int scale(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }

    int metric(int index) const {
        using MetricsForDpiFunction = int (WINAPI*)(int, UINT);
        const auto getMetric = user32Function<MetricsForDpiFunction>("GetSystemMetricsForDpi");
        return getMetric ? getMetric(index, dpi_) : GetSystemMetrics(index);
    }

    void fitToWorkArea(RECT& bounds) const {
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (!GetMonitorInfoW(MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST), &monitor)) return;
        const int width = (std::min)(bounds.right - bounds.left, monitor.rcWork.right - monitor.rcWork.left);
        const int height = (std::min)(bounds.bottom - bounds.top, monitor.rcWork.bottom - monitor.rcWork.top);
        bounds.left = std::clamp(bounds.left, monitor.rcWork.left, monitor.rcWork.right - width);
        bounds.top = std::clamp(bounds.top, monitor.rcWork.top, monitor.rcWork.bottom - height);
        bounds.right = bounds.left + width;
        bounds.bottom = bounds.top + height;
    }

    void layoutControls() {
        for (const auto& item : placements_)
            SetWindowPos(item.control, nullptr, scale(item.x) - scrollX_, scale(item.y) - scrollY_,
                         scale(item.width), scale(item.height), SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(window_, nullptr, TRUE);
    }

    void updateScrollBars() {
        if (updatingScroll_ || !window_) return;
        updatingScroll_ = true;
        RECT client{}; GetClientRect(window_, &client);
        const auto style = static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_STYLE));
        const int fullWidth = client.right + ((style & WS_VSCROLL) ? metric(SM_CXVSCROLL) : 0);
        const int fullHeight = client.bottom + ((style & WS_HSCROLL) ? metric(SM_CYHSCROLL) : 0);
        bool vertical = false, horizontal = false;
        for (int pass = 0; pass < 3; ++pass) {
            vertical = scale(contentHeight) > fullHeight - (horizontal ? metric(SM_CYHSCROLL) : 0);
            horizontal = scale(contentWidth) > fullWidth - (vertical ? metric(SM_CXVSCROLL) : 0);
        }
        ShowScrollBar(window_, SB_VERT, vertical);
        ShowScrollBar(window_, SB_HORZ, horizontal);
        GetClientRect(window_, &client);
        scrollX_ = std::clamp(scrollX_, 0, (std::max)(0, scale(contentWidth) - static_cast<int>(client.right)));
        scrollY_ = std::clamp(scrollY_, 0, (std::max)(0, scale(contentHeight) - static_cast<int>(client.bottom)));
        SCROLLINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        info.nMax = scale(contentHeight) - 1;
        info.nPage = static_cast<UINT>((std::max)(1L, client.bottom));
        info.nPos = scrollY_;
        SetScrollInfo(window_, SB_VERT, &info, TRUE);
        info.nMax = scale(contentWidth) - 1;
        info.nPage = static_cast<UINT>((std::max)(1L, client.right));
        info.nPos = scrollX_;
        SetScrollInfo(window_, SB_HORZ, &info, TRUE);
        layoutControls();
        updatingScroll_ = false;
    }

    void scrollTo(int x, int y) {
        RECT client{}; GetClientRect(window_, &client);
        const int nextX = std::clamp(x, 0, (std::max)(0, scale(contentWidth) - static_cast<int>(client.right)));
        const int nextY = std::clamp(y, 0, (std::max)(0, scale(contentHeight) - static_cast<int>(client.bottom)));
        if (nextX == scrollX_ && nextY == scrollY_) return;
        scrollX_ = nextX; scrollY_ = nextY;
        SetScrollPos(window_, SB_HORZ, scrollX_, TRUE);
        SetScrollPos(window_, SB_VERT, scrollY_, TRUE);
        layoutControls();
    }

    void scroll(bool vertical, UINT code) {
        SCROLLINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = SIF_ALL;
        GetScrollInfo(window_, vertical ? SB_VERT : SB_HORZ, &info);
        int position = vertical ? scrollY_ : scrollX_;
        const int page = (std::max)(scale(32), static_cast<int>(info.nPage) - scale(24));
        switch (code) {
        case SB_LINEUP: position -= scale(32); break;
        case SB_LINEDOWN: position += scale(32); break;
        case SB_PAGEUP: position -= page; break;
        case SB_PAGEDOWN: position += page; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: position = info.nTrackPos; break;
        case SB_TOP: position = 0; break;
        case SB_BOTTOM: position = info.nMax; break;
        default: return;
        }
        scrollTo(vertical ? scrollX_ : position, vertical ? position : scrollY_);
    }

    void revealControl(HWND control) {
        if (GetParent(control) != window_ || !IsWindowVisible(control)) return;
        RECT bounds{}, client{};
        GetWindowRect(control, &bounds);
        MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&bounds), 2);
        GetClientRect(window_, &client);
        const int margin = scale(8);
        int x = scrollX_, y = scrollY_;
        if (bounds.right - bounds.left > client.right - 2 * margin) x += bounds.left - margin;
        else if (bounds.left < margin) x += bounds.left - margin;
        else if (bounds.right > client.right - margin) x += bounds.right - client.right + margin;
        if (bounds.top < margin) y += bounds.top - margin;
        else if (bounds.bottom > client.bottom - margin) y += bounds.bottom - client.bottom + margin;
        scrollTo(x, y);
    }

    static LRESULT CALLBACK controlProcedure(HWND control, UINT message, WPARAM wParam,
                                             LPARAM lParam, UINT_PTR subclassId, DWORD_PTR data) {
        auto* self = reinterpret_cast<MainWindow*>(data);
        if (message == WM_NCDESTROY) RemoveWindowSubclass(control, controlProcedure, subclassId);
        else if (message == WM_SETFOCUS)
            PostMessageW(self->window_, revealFocusMessage, reinterpret_cast<WPARAM>(control), 0);
        else if (message == WM_MOUSEWHEEL) {
            if (control == self->source_ && ComboBox_GetDroppedState(control))
                return DefSubclassProc(control, message, wParam, lParam);
            return SendMessageW(self->window_, message, wParam, lParam);
        } else if (message == WM_KEYDOWN && (wParam == VK_PRIOR || wParam == VK_NEXT)) {
            if (control == self->source_ && ComboBox_GetDroppedState(control))
                return DefSubclassProc(control, message, wParam, lParam);
            self->scroll(true, wParam == VK_PRIOR ? SB_PAGEUP : SB_PAGEDOWN);
            return 0;
        }
        return DefSubclassProc(control, message, wParam, lParam);
    }

    void loadInputs() {
        const auto& preferences = controller_.preferences();
        setNumber(focus_, preferences.pomodoro.focusMs / 60000);
        setNumber(break_, preferences.pomodoro.breakMs / 60000);
        setNumber(cycles_, preferences.pomodoro.cycles);
        Millis seconds = preferences.countdownDurationMs / 1000;
        setNumber(hours_, seconds / 3600); setNumber(minutes_, seconds / 60 % 60); setNumber(seconds_, seconds % 60);
        Button_SetCheck(sound_, preferences.soundEnabled ? BST_CHECKED : BST_UNCHECKED);
        Button_SetCheck(statusCheck_, preferences.showTaskbarStatus ? BST_CHECKED : BST_UNCHECKED);
        ComboBox_SetCurSel(source_, preferences.taskbarSource == TaskbarSource::Pomodoro ? 0 : 1);
    }
    void setNumber(HWND control, Millis value) { SetWindowTextW(control, std::to_wstring(value).c_str()); }
    bool number(HWND control, Millis maximum, Millis& result) {
        wchar_t text[32]{};
        const int length = GetWindowTextW(control, text, 32);
        if (length == 0) return false;
        result = 0;
        for (int i = 0; i < length; ++i) {
            if (text[i] < L'0' || text[i] > L'9') return false;
            const int digit = text[i] - L'0';
            if (result > (maximum - digit) / 10) return false;
            result = result * 10 + digit;
            if (result > maximum) return false;
        }
        return true;
    }
    bool readPomodoro(PomodoroConfig& config) {
        Millis focus, rest, cycles;
        if (!number(focus_, 10080, focus) || !number(break_, 10080, rest) ||
            !number(cycles_, maxCycles, cycles) || !focus || !rest || !cycles) {
            error(L"Enter focus and break durations from 1 to 10,080 minutes and cycles from 1 to 1,000."); return false;
        }
        config = {focus * 60000, rest * 60000, static_cast<int>(cycles)}; return true;
    }
    bool readCountdown(Millis& value) {
        Millis h, m, s;
        if (!number(hours_, 168, h) || !number(minutes_, 59, m) || !number(seconds_, 59, s)) {
            error(L"Enter hours from 0 to 168 and minutes/seconds from 0 to 59."); return false;
        }
        value = ((h * 60 + m) * 60 + s) * 1000;
        if (!validateDuration(value)) { error(L"The total duration must be positive and at most 168 hours."); return false; }
        return true;
    }
    void error(const std::wstring& text) { MessageBoxW(window_, text.c_str(), L"Pomodoro", MB_OK | MB_ICONERROR); }

    std::wstring currentSummary() const {
        const auto& engine = controller_.engine();
        return std::wstring(engine.pomodoro.phase == Phase::Focus ? L"Focus " : L"Break ") +
            duration(engine.pomodoroRemaining(controller_.now())) + L" (" + statusName(engine.pomodoro.status) +
            L"). Timer " + duration(engine.countdownRemaining(controller_.now())) + L" (" + statusName(engine.countdown.status) + L").";
    }

    void showUpdate(const Update& update) {
        if (update.events.empty()) return;
        for (auto& notification : completionNotifications(controller_.engine(), update.events,
                controller_.now(), update.recovery, controller_.preferences().soundEnabled)) {
            notice_ = notification.title + L". " + notification.body;
            outbox_.post(std::move(notification), controller_.now());
        }
    }

    void clearNotification(TimerKind timer) {
        outbox_.cancel(timer);
        // Lazy initialization keeps tests and an unused notification backend
        // from registering a shortcut or protocol handler.
        if ((runtimeIntegration_ || notificationsUsed_) &&
            !notifications_.clear(timer == TimerKind::Pomodoro ? L"pomodoro" : L"countdown"))
            notificationError_ = notifications_.lastError();
    }

    void applyNotificationAction(const NotificationAction& action) {
        if (action.command == NotificationCommand::Open) {
            const int page = action.timer == TimerKind::Pomodoro ? 0 : 1;
            TabCtrl_SetCurSel(tabs_, page); choosePage(page); open();
        } else if (matchesNotificationSession(controller_.engine(), action)) {
            if (action.timer == TimerKind::Pomodoro) controller_.resetPomodoro();
            else controller_.resetCountdown();
            clearNotification(action.timer);
            notice_ = action.timer == TimerKind::Pomodoro ? L"Pomodoro stopped." : L"Timer stopped.";
        }
    }

    void refresh() {
        const auto& engine = controller_.engine();
        const auto& p = engine.pomodoro;
        const auto& c = engine.countdown;
        SetWindowTextW(pomodoroTime_, duration(engine.pomodoroRemaining(controller_.now())).c_str());
        SetWindowTextW(countdownTime_, duration(engine.countdownRemaining(controller_.now())).c_str());
        std::wstring phase = std::wstring(p.phase == Phase::Focus ? L"Focus" : L"Break") + L" — cycle " +
            std::to_wstring(p.cycle) + L" / " + std::to_wstring(p.config.cycles) + L" · " + statusName(p.status);
        SetWindowTextW(pomodoroPhase_, phase.c_str());
        const std::wstring captured = L"Session setup: " + duration(p.config.focusMs) + L" focus, " +
            duration(p.config.breakMs) + L" break, " + std::to_wstring(p.config.cycles) + L" cycles.\r\nSaved setup changes apply to the next new session.";
        SetWindowTextW(pomodoroCaptured_, captured.c_str());
        const std::wstring timerStatus = std::wstring(statusName(c.status)) + L" · session duration " + duration(c.durationMs);
        SetWindowTextW(countdownStatus_, timerStatus.c_str());
        SetWindowTextW(pomodoroAction_, p.status == Status::Running ? L"Pause" : p.status == Status::Paused ? L"Resume" : L"Start");
        SetWindowTextW(countdownAction_, c.status == Status::Running ? L"Pause" : c.status == Status::Paused ? L"Resume" : L"Start");
        EnableWindow(skip_, active(p.status));

        bool usePomodoro = controller_.preferences().taskbarSource == TaskbarSource::Pomodoro;
        if (!active(usePomodoro ? p.status : c.status) && active(usePomodoro ? c.status : p.status)) usePomodoro = !usePomodoro;
        const auto remaining = usePomodoro ? engine.pomodoroRemaining(controller_.now()) : engine.countdownRemaining(controller_.now());
        const auto full = usePomodoro ? p.phaseDurationMs : c.durationMs;
        const auto selectedStatus = usePomodoro ? p.status : c.status;
        shell_->setTaskbar(static_cast<ULONGLONG>((std::max<Millis>)(0, full - remaining)),
                           static_cast<ULONGLONG>(full), selectedStatus == Status::Paused, active(selectedStatus));
        const std::wstring title = (usePomodoro ? std::wstring(p.phase == Phase::Focus ? L"Focus " : L"Break ") : std::wstring(L"Timer ")) +
            duration(remaining) + (usePomodoro ? L" — " + std::to_wstring(p.cycle) + L"/" + std::to_wstring(p.config.cycles) : L"") + L" — Pomodoro";
        SetWindowTextW(window_, title.c_str());
        shell_->updateTooltip(currentSummary());
        std::wstring panelWarning;
        if (taskbarPanel_) {
            const bool showPanel = controller_.preferences().showTaskbarStatus && active(selectedStatus);
            const std::wstring panelPhase = p.phase == Phase::Focus ? L"Focus " : L"Break ";
            const std::wstring panelCycle = (p.status == Status::Paused ? L"Paused · " : L"Cycle ") +
                std::to_wstring(p.cycle) + L" / " + std::to_wstring(p.config.cycles);
            taskbarPanel_->update((usePomodoro ? panelPhase : L"Timer ") + duration(remaining),
                usePomodoro ? panelCycle : selectedStatus == Status::Paused ? L"Paused" : L"Countdown", showPanel);
            if (showPanel && !taskbarPanel_->available()) panelWarning = taskbarPanel_->lastError();
        }
        outbox_.discardStale(engine);
        outbox_.dispatch(controller_.now(), [this](const Notification& notification) {
            notificationsUsed_ = true;
            if (notifications_.show(notification.xml, notification.tag)) {
                notificationError_.clear();
                return true;
            }
            notificationError_ = notifications_.lastError();
            return false;
        });
        const bool needsScheduler = controller_.running() || outbox_.pending();
        if (needsScheduler && !schedulerRunning_) {
            if (SetTimer(window_, schedulerId, 1000, nullptr)) schedulerRunning_ = true;
            else {
                notice_ = L"Windows could not start the display scheduler. Reopen the window to refresh timer state.";
                open();
            }
        } else if (!needsScheduler && schedulerRunning_) {
            KillTimer(window_, schedulerId); schedulerRunning_ = false;
        }
        const std::wstring message = !controller_.warning().empty() ? L"Storage/recovery: " + wide(controller_.warning()) :
            !notificationError_.empty() ? L"Notifications: " + notificationError_ :
            !panelWarning.empty() ? L"Taskbar status: " + panelWarning : notice_;
        SetWindowTextW(status_, message.c_str());
        if (!controller_.warning().empty() && controller_.warning() != lastStorageWarning_) {
            lastStorageWarning_ = controller_.warning();
            open();
        }
    }

    void open() {
        ShowWindow(window_, IsIconic(window_) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(window_);
    }
    void backgroundWindow() {
        if (shell_->trayInstalled()) ShowWindow(window_, SW_HIDE);
        else ShowWindow(window_, SW_MINIMIZE);
    }

    void trayMenu() {
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING, OpenWindow, L"Open");
        AppendMenuW(menu, MF_STRING, OpenSettings, L"Settings");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        const auto& engine = controller_.engine();
        AppendMenuW(menu, MF_STRING | (active(engine.pomodoro.status) ? 0 : MF_GRAYED), TogglePomodoro,
                    engine.pomodoro.status == Status::Paused ? L"Resume Pomodoro" : L"Pause Pomodoro");
        AppendMenuW(menu, MF_STRING, PomodoroReset, L"Reset Pomodoro");
        AppendMenuW(menu, MF_STRING | (active(engine.countdown.status) ? 0 : MF_GRAYED), ToggleCountdown,
                    engine.countdown.status == Status::Paused ? L"Resume timer" : L"Pause timer");
        AppendMenuW(menu, MF_STRING, CountdownReset, L"Reset timer");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ExitApplication, L"Exit");
        POINT cursor{}; GetCursorPos(&cursor);
        SetForegroundWindow(window_);
        const int selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                            cursor.x, cursor.y, 0, window_, nullptr);
        DestroyMenu(menu);
        PostMessageW(window_, WM_NULL, 0, 0);
        if (selected) command(selected);
    }

    void command(int id) {
        showUpdate(controller_.poll());
        switch (id) {
        case OpenWindow: open(); break;
        case OpenSettings: TabCtrl_SetCurSel(tabs_, 2); choosePage(2); open(); break;
        case PomodoroAction:
            if (active(controller_.engine().pomodoro.status)) controller_.togglePomodoro();
            else {
                PomodoroConfig config;
                if (readPomodoro(config)) {
                    auto prefs = controller_.preferences(); prefs.pomodoro = config;
                    controller_.savePreferences(prefs);
                    if (!controller_.startPomodoro(config)) error(L"Could not start with these settings.");
                    else clearNotification(TimerKind::Pomodoro);
                }
            }
            break;
        case CountdownAction:
            if (active(controller_.engine().countdown.status)) controller_.toggleCountdown();
            else {
                Millis durationMs;
                if (readCountdown(durationMs)) {
                    auto prefs = controller_.preferences(); prefs.countdownDurationMs = durationMs;
                    controller_.savePreferences(prefs);
                    if (!controller_.startCountdown(durationMs)) error(L"Could not start with this duration.");
                    else clearNotification(TimerKind::Countdown);
                }
            }
            break;
        case TogglePomodoro: controller_.togglePomodoro(); break;
        case ToggleCountdown: controller_.toggleCountdown(); break;
        case PomodoroSkip: controller_.skipPomodoro(); clearNotification(TimerKind::Pomodoro); break;
        case PomodoroReset:
            controller_.resetPomodoro(); clearNotification(TimerKind::Pomodoro);
            notice_ = L"Pomodoro reset to the saved setup."; break;
        case CountdownReset:
            controller_.resetCountdown(); clearNotification(TimerKind::Countdown);
            notice_ = L"Timer reset to the saved duration."; break;
        case SavePomodoro: {
            PomodoroConfig config;
            if (readPomodoro(config)) {
                auto prefs = controller_.preferences(); prefs.pomodoro = config;
                if (controller_.savePreferences(prefs)) notice_ = L"Setup saved. Active sessions keep their captured settings.";
            }
            break;
        }
        case SaveCountdown: {
            Millis durationMs;
            if (readCountdown(durationMs)) {
                auto prefs = controller_.preferences(); prefs.countdownDurationMs = durationMs;
                if (controller_.savePreferences(prefs)) notice_ = L"Duration saved for the next new timer.";
            }
            break;
        }
        case SaveSettings: {
            auto prefs = controller_.preferences();
            prefs.soundEnabled = Button_GetCheck(sound_) == BST_CHECKED;
            prefs.startAtSignIn = Button_GetCheck(startup_) == BST_CHECKED;
            prefs.showTaskbarStatus = Button_GetCheck(statusCheck_) == BST_CHECKED;
            prefs.taskbarSource = ComboBox_GetCurSel(source_) == 0 ? TaskbarSource::Pomodoro : TaskbarSource::Countdown;
            std::wstring startupError;
            // Reconcile the registry on every explicit save: the executable may
            // have moved, or another process may have changed the Run value.
            if (!setStartAtSignIn(prefs.startAtSignIn, startupError)) {
                Button_SetCheck(startup_, startupActual_ ? BST_CHECKED : BST_UNCHECKED);
                error(startupError); break;
            }
            startupActual_ = prefs.startAtSignIn;
            if (controller_.savePreferences(prefs)) notice_ = L"Settings saved.";
            break;
        }
        case TestNotification: {
            const auto notification = testNotification(Button_GetCheck(sound_) == BST_CHECKED);
            notificationsUsed_ = true;
            if (notifications_.show(notification.xml, notification.tag)) {
                notificationError_.clear();
                notice_ = L"Windows accepted the test notification request. Notification settings and Focus assist control banner visibility.";
            } else {
                notificationError_ = notifications_.lastError();
                notice_ = L"Test notification failed. " + notificationError_;
            }
            break;
        }
        case ExitApplication:
            if (!controller_.save()) {
                const auto response = MessageBoxW(window_, (L"Timer state could not be saved.\r\n" + wide(controller_.warning()) +
                    L"\r\n\r\nExit anyway?").c_str(), L"Pomodoro", MB_YESNO | MB_ICONWARNING);
                if (response != IDYES) break;
            }
            DestroyWindow(window_); return;
        default: break;
        }
        refresh();
    }

    static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wParam, lParam);
        try { return self->handleMessage(message, wParam, lParam); }
        catch (const std::exception& error) {
            MessageBoxW(window, wide(error.what()).c_str(), L"Pomodoro error", MB_OK | MB_ICONERROR);
            return message == WM_CREATE ? -1 : 0;
        }
    }

    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        if (taskbarCreated_ != 0 && message == taskbarCreated_ && shell_) {
            if (!shell_->restoreTray()) { notice_ = L"Could not restore the tray icon after Explorer restarted."; open(); }
            if (taskbarPanel_) taskbarPanel_->explorerRestarted();
            shell_->initializeTaskbar(); refresh(); return 0;
        }
        if (taskbarButtonCreated_ != 0 && message == taskbarButtonCreated_ && shell_) { shell_->initializeTaskbar(); refresh(); return 0; }
        switch (message) {
        case WM_CREATE:
            taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
            taskbarButtonCreated_ = RegisterWindowMessageW(L"TaskbarButtonCreated");
            buildControls(); return 0;
        case activateMessage: open(); showUpdate(controller_.poll()); refresh(); return 0;
        case notificationFailureMessage: {
            const auto error = notifications_.takeFailure();
            if (!error.empty()) notificationError_ = error;
            refresh(); return 0;
        }
        case WM_COPYDATA: {
            const auto data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
            if (!data || data->dwData != notificationCopyData || !data->lpData ||
                data->cbData < sizeof(wchar_t) || data->cbData > 512 || data->cbData % sizeof(wchar_t) != 0) return FALSE;
            const auto text = static_cast<const wchar_t*>(data->lpData);
            const auto count = data->cbData / sizeof(wchar_t);
            if (text[count - 1] != L'\0') return FALSE;
            const auto action = parseNotificationAction(std::wstring(text, count - 1));
            if (!action) return FALSE;
            auto update = controller_.poll();
            if (action->command == NotificationCommand::Stop && matchesNotificationSession(controller_.engine(), *action)) {
                update.events.erase(std::remove_if(update.events.begin(), update.events.end(),
                    [&action](const CompletionEvent& e) { return e.timer == action->timer; }), update.events.end());
            }
            applyNotificationAction(*action);
            showUpdate(update); refresh(); return TRUE;
        }
        case revealFocusMessage:
            if (GetFocus() == reinterpret_cast<HWND>(wParam)) revealControl(reinterpret_cast<HWND>(wParam));
            return 0;
        case WM_TIMER: if (wParam == schedulerId) { showUpdate(controller_.poll()); refresh(); } return 0;
        case WM_COMMAND:
            if (HIWORD(wParam) == BN_CLICKED) command(LOWORD(wParam));
            return 0;
        case WM_NOTIFY:
            if (reinterpret_cast<NMHDR*>(lParam)->idFrom == Tabs && reinterpret_cast<NMHDR*>(lParam)->code == TCN_SELCHANGE)
                choosePage(TabCtrl_GetCurSel(tabs_));
            return 0;
        case trayMessage:
            if (LOWORD(lParam) == NIN_SELECT || LOWORD(lParam) == NIN_KEYSELECT || LOWORD(lParam) == NIN_BALLOONUSERCLICK) {
                open(); showUpdate(controller_.poll()); refresh();
            } else if (LOWORD(lParam) == WM_CONTEXTMENU) trayMenu(); return 0;
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED && shell_ && shell_->trayInstalled())
                ShowWindow(window_, SW_HIDE);
            if (wParam != SIZE_MINIMIZED) updateScrollBars();
            return 0;
        case WM_VSCROLL: scroll(true, LOWORD(wParam)); return 0;
        case WM_HSCROLL: scroll(false, LOWORD(wParam)); return 0;
        case WM_MOUSEWHEEL: {
            wheelDelta_ += GET_WHEEL_DELTA_WPARAM(wParam);
            const int detents = wheelDelta_ / WHEEL_DELTA;
            wheelDelta_ %= WHEEL_DELTA;
            UINT lines = 3;
            SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
            if (lines == WHEEL_PAGESCROLL) {
                for (int i = 0; i < (detents < 0 ? -detents : detents); ++i)
                    scroll(true, detents > 0 ? SB_PAGEUP : SB_PAGEDOWN);
            } else {
                const int lineCount = static_cast<int>((std::min)(lines, 100U));
                scrollTo(scrollX_, scrollY_ - detents * scale(24) * lineCount);
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == VK_PRIOR || wParam == VK_NEXT) scroll(true, wParam == VK_PRIOR ? SB_PAGEUP : SB_PAGEDOWN);
            else if (wParam == VK_HOME) scroll(true, SB_TOP);
            else if (wParam == VK_END) scroll(true, SB_BOTTOM);
            else return DefWindowProcW(window_, message, wParam, lParam);
            return 0;
        case WM_DPICHANGED: {
            setDpi(HIWORD(wParam));
            RECT bounds = *reinterpret_cast<RECT*>(lParam);
            fitToWorkArea(bounds);
            SetWindowPos(window_, nullptr, bounds.left, bounds.top, bounds.right - bounds.left,
                         bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE);
            revealControl(GetFocus());
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto info = reinterpret_cast<MINMAXINFO*>(lParam);
            RECT bounds{0, 0, scale(600), scale(350)};
            RECT current{}; GetWindowRect(window_, &current);
            bounds.left += current.left; bounds.right += current.left;
            bounds.top += current.top; bounds.bottom += current.top;
            fitToWorkArea(bounds);
            info->ptMinTrackSize = {bounds.right - bounds.left, bounds.bottom - bounds.top};
            return 0;
        }
        case WM_POWERBROADCAST:
            if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) { showUpdate(controller_.poll(true)); refresh(); }
            return TRUE;
        case WM_TIMECHANGE: showUpdate(controller_.poll()); refresh(); return 0;
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            if (taskbarPanel_) { showUpdate(controller_.poll()); refresh(); }
            return 0;
        case WM_QUERYENDSESSION: showUpdate(controller_.poll()); controller_.save(); return TRUE;
        case WM_ENDSESSION: if (wParam) { controller_.save(); DestroyWindow(window_); } return 0;
        case WM_CLOSE:
            command(ExitApplication);
            return 0;
        case WM_DESTROY:
            if (schedulerRunning_) KillTimer(window_, schedulerId);
            notifications_.setFailureTarget(nullptr, 0);
            taskbarPanel_.reset();
            shell_.reset(); PostQuitMessage(0); return 0;
        default: return DefWindowProcW(window_, message, wParam, lParam);
        }
    }

    HINSTANCE instance_;
    HWND window_ = nullptr;
    Controller controller_;
    std::unique_ptr<ShellIntegration> shell_;
    std::unique_ptr<TaskbarStatusPanel> taskbarPanel_;
    WindowsNotifications notifications_;
    NotificationOutbox outbox_;
    bool notificationsUsed_ = false;
    bool runtimeIntegration_ = false;
    std::wstring notificationError_;
    std::vector<Placement> placements_;
    HFONT font_ = nullptr, largeFont_ = nullptr;
    UINT dpi_ = 96, taskbarCreated_ = 0, taskbarButtonCreated_ = 0;
    int page_ = 0, scrollX_ = 0, scrollY_ = 0, wheelDelta_ = 0;
    bool schedulerRunning_ = false, startupActual_ = false, updatingScroll_ = false;
    std::wstring notice_ = L"Ready. Minimize hides the app; Close and Exit save and quit.";
    std::string lastStorageWarning_;
    HWND tabs_{}, focus_{}, break_{}, cycles_{}, pomodoroTime_{}, pomodoroPhase_{}, pomodoroCaptured_{}, pomodoroAction_{}, skip_{};
    HWND hours_{}, minutes_{}, seconds_{}, countdownTime_{}, countdownStatus_{}, countdownAction_{};
    HWND sound_{}, startup_{}, statusCheck_{}, source_{}, status_{};
};

struct HandleCloser { void operator()(void* handle) const { if (handle) CloseHandle(handle); } };
using UniqueHandle = std::unique_ptr<void, HandleCloser>;
struct ComLifetime {
    HRESULT status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComLifetime() { if (SUCCEEDED(status)) CoUninitialize(); }
};
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    bool background = false;
    std::optional<NotificationAction> activation;
    std::wstring activationUri;
    bool invalidActivation = false;
    if (arguments) {
        for (int i = 1; i < argumentCount; ++i) {
            if (std::wstring(arguments[i]) == L"--background") background = true;
            else if (std::wstring(arguments[i]) == L"--notification") {
                if (++i < argumentCount && !activation) {
                    activationUri = arguments[i];
                    activation = parseNotificationAction(activationUri);
                    if (!activation) invalidActivation = true;
                } else invalidActivation = true;
            }
        }
        LocalFree(arguments);
    }
    if (invalidActivation) return 1;
    if (activation && activation->command == NotificationCommand::Stop) background = true;
    UniqueHandle mutex(CreateMutexW(nullptr, TRUE, L"Local\\PomodoroNative.SingleInstance.v1"));
    if (!mutex) { MessageBoxW(nullptr, L"Could not create the single-instance guard.", L"Pomodoro", MB_OK | MB_ICONERROR); return 1; }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = nullptr;
        for (int i = 0; i < 50 && !existing; ++i) {
            existing = FindWindowW(windowClass, nullptr);
            if (!existing) Sleep(100);
        }
        if (existing) {
            DWORD processId = 0;
            GetWindowThreadProcessId(existing, &processId);
            if (!activation || activation->command == NotificationCommand::Open) AllowSetForegroundWindow(processId);
            if (activation) {
                COPYDATASTRUCT data{notificationCopyData,
                    static_cast<DWORD>((activationUri.size() + 1) * sizeof(wchar_t)), activationUri.data()};
                DWORD_PTR accepted = 0;
                return SendMessageTimeoutW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &accepted) && accepted ? 0 : 1;
            }
            PostMessageW(existing, activateMessage, 0, 0);
        }
        return 0;
    }
    try {
        ComLifetime com;
        SetCurrentProcessExplicitAppUserModelID(L"PomodoroNative.Desktop");
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TAB_CLASSES | ICC_STANDARD_CLASSES};
        if (!InitCommonControlsEx(&controls)) throw std::runtime_error("Could not initialize Windows controls.");
        StateStore store(dataDirectory());
        WindowsClock clock;
        MainWindow app(instance, store, clock);
        if (!app.create()) throw std::runtime_error("Could not create the Pomodoro window.");
        app.begin(background, activation);
        MSG message{};
        int result;
        while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            if (!IsDialogMessageW(app.handle(), &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        return result == -1 ? 1 : static_cast<int>(message.wParam);
    } catch (const std::exception& error) {
        MessageBoxW(nullptr, wide(error.what()).c_str(), L"Pomodoro", MB_OK | MB_ICONERROR); return 1;
    }
}
