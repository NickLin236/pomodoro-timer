// Compile the actual command handler into this Windows-only integration test.
// No message loop, tray registration, sign-in launch, or real user Run key is
// used: the window stays hidden and HKCU is redirected for this process.
#include "../src/ui/main_window.cpp"
#include "pomodoro/taskbar_status_panel.h"
#include "pomodoro/taskbar_obstacles.h"

#include <iostream>
#include <optional>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class RegistrySandbox {
public:
    RegistrySandbox() {
        name_ = L"Software\\PomodoroNative.UiTests." + std::to_wstring(GetCurrentProcessId()) +
                L"." + std::to_wstring(GetTickCount64());
        require(RegCreateKeyExW(HKEY_CURRENT_USER, name_.c_str(), 0, nullptr, 0,
                               KEY_ALL_ACCESS, nullptr, &key_, nullptr) == ERROR_SUCCESS,
                "Could not create isolated registry test key");
        const auto status = RegOverridePredefKey(HKEY_CURRENT_USER, key_);
        if (status != ERROR_SUCCESS) {
            RegCloseKey(key_); key_ = nullptr;
            RegDeleteTreeW(HKEY_CURRENT_USER, name_.c_str());
            throw std::runtime_error("Could not redirect HKCU for test process");
        }
        overridden_ = true;
    }
    ~RegistrySandbox() {
        // Restore HKCU before deleting the real sandbox root, even after a
        // failed assertion. The application's real Run key is never opened.
        if (overridden_) RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        if (key_) RegCloseKey(key_);
        RegDeleteTreeW(HKEY_CURRENT_USER, name_.c_str());
    }
    RegistrySandbox(const RegistrySandbox&) = delete;
    RegistrySandbox& operator=(const RegistrySandbox&) = delete;

private:
    HKEY key_ = nullptr;
    std::wstring name_;
    bool overridden_ = false;
};

constexpr wchar_t runKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

void putStartup(const std::wstring& command) {
    HKEY key = nullptr;
    require(RegCreateKeyExW(HKEY_CURRENT_USER, runKey, 0, nullptr, 0,
                           KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS,
            "Could not create sandbox Run key");
    const auto status = RegSetValueExW(key, L"PomodoroNative", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(command.c_str()),
        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    require(status == ERROR_SUCCESS, "Could not write sandbox startup value");
}

std::optional<std::wstring> readStartup() {
    HKEY key = nullptr;
    auto status = RegOpenKeyExW(HKEY_CURRENT_USER, runKey, 0, KEY_QUERY_VALUE, &key);
    if (status == ERROR_FILE_NOT_FOUND) return std::nullopt;
    require(status == ERROR_SUCCESS, "Could not read sandbox Run key");
    wchar_t text[32768]{};
    DWORD type = 0, size = sizeof(text);
    status = RegQueryValueExW(key, L"PomodoroNative", nullptr, &type,
                             reinterpret_cast<BYTE*>(text), &size);
    RegCloseKey(key);
    if (status == ERROR_FILE_NOT_FOUND) return std::nullopt;
    require(status == ERROR_SUCCESS && type == REG_SZ && size <= sizeof(text),
            "Invalid sandbox startup value");
    text[std::size(text) - 1] = L'\0';
    return std::wstring(text);
}

void removeStartup() {
    HKEY key = nullptr;
    require(RegOpenKeyExW(HKEY_CURRENT_USER, runKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS,
            "Could not open sandbox Run key for external removal");
    const auto status = RegDeleteValueW(key, L"PomodoroNative");
    RegCloseKey(key);
    require(status == ERROR_SUCCESS, "Could not remove sandbox startup value");
}

class TemporaryData {
public:
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("pomodoro-ui-tests-" + std::to_string(GetCurrentProcessId()) + "-" +
         std::to_string(GetTickCount64()));
    TemporaryData() { require(std::filesystem::create_directory(path), "Could not create UI test data directory"); }
    ~TemporaryData() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};

class HiddenWindow {
public:
    HiddenWindow(MainWindow& app) : app_(app) { require(app_.create(), "Could not create hidden test window"); }
    ~HiddenWindow() { if (IsWindow(app_.handle())) DestroyWindow(app_.handle()); }
private:
    MainWindow& app_;
};

constexpr UINT syntheticPanelCallback = WM_APP + 80;
constexpr wchar_t syntheticTaskbarProperty[] = L"PomodoroNative.SyntheticTaskbar";
constexpr wchar_t foreignTaskbarClass[] = L"PomodoroNative.ForeignTaskbarTest";

struct PanelCallbacks {
    int open = 0;
    int contextMenu = 0;
};

LRESULT CALLBACK panelHostProcedure(HWND window, UINT message, WPARAM wParam,
                                    LPARAM lParam, UINT_PTR, DWORD_PTR data) {
    auto& callbacks = *reinterpret_cast<PanelCallbacks*>(data);
    if (message == syntheticPanelCallback) {
        if (LOWORD(lParam) == NIN_SELECT) ++callbacks.open;
        if (LOWORD(lParam) == WM_CONTEXTMENU) ++callbacks.contextMenu;
        return 0;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

class SyntheticTaskbar {
public:
    PanelCallbacks callbacks;
    HWND window = nullptr;
    HWND taskList = nullptr;
    HWND monitorContainer = nullptr;
    HWND trafficMonitor = nullptr;
    HWND notificationArea = nullptr;

    SyntheticTaskbar() {
        window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            L"STATIC", L"Pomodoro synthetic taskbar", WS_POPUP | WS_BORDER,
            20, 20, 1000, 48, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(window != nullptr, "Could not create synthetic taskbar host");
        require(SetPropW(window, syntheticTaskbarProperty,
            reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(GetCurrentProcessId()))) != FALSE,
            "Could not mark synthetic taskbar host");
        taskList = CreateWindowExW(0, L"STATIC", L"Task-list placeholder", WS_CHILD | WS_VISIBLE,
            0, 0, 1000, 48, window, nullptr, GetModuleHandleW(nullptr), nullptr);
        monitorContainer = CreateWindowExW(0, L"STATIC", L"Monitor container", WS_CHILD | WS_VISIBLE,
            800, 0, 160, 46, window, nullptr, GetModuleHandleW(nullptr), nullptr);
        trafficMonitor = CreateWindowExW(0, L"STATIC", L"TrafficMonitorTaskbarWindow", WS_CHILD | WS_VISIBLE,
            0, 0, 160, 46, monitorContainer, nullptr, GetModuleHandleW(nullptr), nullptr);
        WNDCLASSW notificationType{};
        notificationType.lpfnWndProc = DefWindowProcW;
        notificationType.hInstance = GetModuleHandleW(nullptr);
        notificationType.lpszClassName = L"TrayNotifyWnd";
        if (!GetClassInfoW(notificationType.hInstance, notificationType.lpszClassName, &notificationType)) {
            notificationType.lpfnWndProc = DefWindowProcW;
            notificationType.hInstance = GetModuleHandleW(nullptr);
            notificationType.lpszClassName = L"TrayNotifyWnd";
            require(RegisterClassW(&notificationType) != 0, "Could not register synthetic notification-area class");
        }
        notificationArea = CreateWindowExW(0, L"TrayNotifyWnd", L"Notification area", WS_CHILD | WS_VISIBLE,
            960, 0, 38, 46, window, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!taskList || !monitorContainer || !trafficMonitor || !notificationArea ||
            !SetWindowSubclass(window, panelHostProcedure, 1, reinterpret_cast<DWORD_PTR>(&callbacks))) {
            DestroyWindow(window);
            window = nullptr;
            throw std::runtime_error("Could not initialize synthetic taskbar children");
        }
        ShowWindow(window, SW_SHOWNOACTIVATE);
    }

    ~SyntheticTaskbar() {
        if (window && IsWindow(window)) {
            RemoveWindowSubclass(window, panelHostProcedure, 1);
            DestroyWindow(window);
        }
    }

    void dispatchCallbacks() const {
        MSG message{};
        while (PeekMessageW(&message, window, syntheticPanelCallback, syntheticPanelCallback, PM_REMOVE))
            DispatchMessageW(&message);
    }
};

LRESULT CALLBACK foreignTaskbarProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_GETTEXT) {
        constexpr wchar_t caption[] = L"TrafficMonitorTaskbarWindow";
        if (!wParam || !lParam) return 0;
        const auto length = (std::min)(static_cast<std::size_t>(wParam) - 1, std::size(caption) - 1);
        auto* result = reinterpret_cast<wchar_t*>(lParam);
        std::copy_n(caption, length, result);
        result[length] = L'\0';
        return static_cast<LRESULT>(length);
    }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wParam, lParam);
}

int foreignTaskbarHelper(int count, char** arguments) {
    if (count != 4) return 2;
    const HWND parent = reinterpret_cast<HWND>(static_cast<UINT_PTR>(std::stoull(arguments[2])));
    DWORD parentProcess = 0;
    GetWindowThreadProcessId(parent, &parentProcess);
    if (!parentProcess || GetPropW(parent, syntheticTaskbarProperty) !=
            reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(parentProcess))) return 3;
    const std::string narrowEvent(arguments[3]);
    const std::wstring eventName(narrowEvent.begin(), narrowEvent.end());
    const auto prefix = L"Local\\PomodoroNative.ForeignTaskbarTest." + std::to_wstring(parentProcess) + L".";
    if (eventName.compare(0, prefix.size(), prefix) != 0) return 4;
    UniqueHandle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName.c_str()));
    if (!ready) return 5;
    WNDCLASSW type{};
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpfnWndProc = foreignTaskbarProcedure;
    type.lpszClassName = foreignTaskbarClass;
    if (!RegisterClassW(&type)) return 6;
    // Leave the system caption cache empty. A foreign GetWindowText call sees
    // no caption, while a bounded WM_GETTEXT request reaches the custom text.
    HWND child = CreateWindowExW(WS_EX_NOPARENTNOTIFY | WS_EX_NOACTIVATE,
        foreignTaskbarClass, L"", WS_CHILD | WS_VISIBLE,
        420, 0, 150, 46, parent, nullptr, type.hInstance, nullptr);
    if (!child) return 7;
    SetEvent(ready.get());
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    return 0;
}

DWORD waitForTestObject(HANDLE object, DWORD timeout) {
    const auto started = GetTickCount64();
    for (;;) {
        const auto elapsed = GetTickCount64() - started;
        if (elapsed >= timeout) return WAIT_TIMEOUT;
        const DWORD remaining = timeout - static_cast<DWORD>(elapsed);
        const DWORD result = MsgWaitForMultipleObjects(1, &object, FALSE, remaining, QS_ALLINPUT);
        if (result != WAIT_OBJECT_0 + 1) return result;
        // Cross-process child creation/destruction can synchronously query its
        // parent. Keep the test's UI thread responsive during bounded waits.
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(message.wParam));
                return WAIT_FAILED;
            }
            TranslateMessage(&message); DispatchMessageW(&message);
        }
    }
}

class ForeignTaskbarChild {
public:
    explicit ForeignTaskbarChild(HWND host) : host_(host) {
        static unsigned serial = 0;
        const auto name = L"Local\\PomodoroNative.ForeignTaskbarTest." + std::to_wstring(GetCurrentProcessId()) +
            L"." + std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(++serial);
        UniqueHandle ready(CreateEventW(nullptr, TRUE, FALSE, name.c_str()));
        require(ready != nullptr, "Could not create foreign taskbar readiness event");
        std::vector<wchar_t> executable(32768);
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        require(length != 0 && length < executable.size(), "Could not locate foreign taskbar test executable");
        auto command = L"\"" + std::wstring(executable.data(), length) + L"\" --synthetic-taskbar-child " +
            std::to_wstring(reinterpret_cast<UINT_PTR>(host_)) + L" " + name;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &process) != FALSE, "Could not launch foreign taskbar test process");
        process_.reset(process.hProcess);
        CloseHandle(process.hThread);
        if (waitForTestObject(ready.get(), 5000) != WAIT_OBJECT_0) {
            stop();
            throw std::runtime_error("Foreign taskbar test process did not become ready");
        }
        window = FindWindowExW(host_, nullptr, foreignTaskbarClass, nullptr);
        if (!window) {
            stop();
            throw std::runtime_error("Could not find foreign synthetic taskbar child");
        }
    }
    ~ForeignTaskbarChild() { stop(); }
    HWND window = nullptr;
private:
    void stop() {
        if (!process_) return;
        if (!window) window = FindWindowExW(host_, nullptr, foreignTaskbarClass, nullptr);
        if (window) PostMessageW(window, WM_CLOSE, 0, 0);
        if (waitForTestObject(process_.get(), 2000) != WAIT_OBJECT_0) {
            // This handle belongs only to the helper started by this fixture.
            TerminateProcess(process_.get(), 1);
            WaitForSingleObject(process_.get(), 1000);
        }
        process_.reset();
    }
    HWND host_;
    UniqueHandle process_;
};

RECT screenBounds(HWND window) {
    RECT bounds{};
    require(GetWindowRect(window, &bounds) != FALSE, "Could not read synthetic window bounds");
    return bounds;
}

void requireSameBounds(HWND window, const RECT& expected, const char* failure) {
    const auto actual = screenBounds(window);
    require(EqualRect(&actual, &expected) != FALSE, failure);
}

RECT clientScreenBounds(HWND window) {
    RECT bounds{};
    require(GetClientRect(window, &bounds) != FALSE, "Could not read synthetic taskbar client area");
    MapWindowPoints(window, HWND_DESKTOP, reinterpret_cast<POINT*>(&bounds), 2);
    return bounds;
}

std::wstring windowCaption(HWND window) {
    std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(window)) + 1, L'\0');
    const auto length = GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
    text.resize(static_cast<std::size_t>(length));
    return text;
}

void notificationXmlUsesWindowsRuntimeWithoutSending() {
    std::wstring error;
    require(validateNotificationXml(testNotification().xml, error),
            "Windows Runtime rejected the test notification XML or toast ABI");
    TimerEngine engine;
    require(engine.startPomodoro({2'000, 1'000, 1}, 0, "native-smoke-pomo"),
            "Could not start the notification smoke-test Pomodoro");
    require(engine.startCountdown(2'000, 0, "native-smoke-timer"),
            "Could not start the notification smoke-test countdown");
    const auto events = engine.advance(2'000);
    const auto notices = completionNotifications(engine, events, 2'000, false, false);
    require(notices.size() == 2, "Notification smoke test did not produce both timer types");
    for (const auto& notice : notices) {
        error.clear();
        require(validateNotificationXml(notice.xml, error),
                "Windows Runtime rejected a silent completion notification XML or toast ABI");
    }
    error.clear();
    require(!validateNotificationXml(L"<toast><visual>", error),
            "Windows Runtime accepted malformed notification XML");
    require(!error.empty(), "Malformed notification XML did not provide a diagnostic");
}

void taskbarStatusUsesOnlyItsSyntheticHost() {
    SyntheticTaskbar host;
    const auto hostBefore = screenBounds(host.window);
    const auto hostClient = clientScreenBounds(host.window);
    const auto taskListBefore = screenBounds(host.taskList);
    const auto monitorBefore = screenBounds(host.trafficMonitor);
    HWND panelWindow = nullptr;
    {
        TaskbarStatusPanel panel(GetModuleHandleW(nullptr), host.window,
                                syntheticPanelCallback, host.window);
        panel.update(L"Focus 24:59", L"Cycle 1 / 4", true);
        panelWindow = panel.handle();
        require(panel.available() && IsWindow(panelWindow), "Status panel did not attach to the synthetic taskbar");
        require(GetParent(panelWindow) == host.window, "Status panel attached outside its supplied taskbar host");
        require(IsWindowVisible(panelWindow) != FALSE, "Active status panel was not visible");
        const auto panelBounds = screenBounds(panelWindow);
        require(panelBounds.right <= monitorBefore.left && panelBounds.left >= hostBefore.left,
                "Status panel was not placed to the left of Traffic Monitor");
        require(panelBounds.top >= hostClient.top && panelBounds.bottom <= hostClient.bottom,
                "Status panel extends outside the taskbar client area vertically");
        require(GetTopWindow(host.window) == panelWindow,
                "Status panel is behind the task-list container");
        requireSameBounds(host.window, hostBefore, "Status panel resized or moved the taskbar host");
        requireSameBounds(host.taskList, taskListBefore, "Status panel resized or moved the task list");
        requireSameBounds(host.trafficMonitor, monitorBefore, "Status panel resized or moved Traffic Monitor");
        auto caption = windowCaption(panelWindow);
        require(caption.find(L"Focus 24:59") != std::wstring::npos && caption.find(L"Cycle 1 / 4") != std::wstring::npos,
                "Status panel did not expose its current phase/countdown text");

        SendMessageW(panelWindow, WM_LBUTTONUP, 0, MAKELPARAM(5, 5));
        SendMessageW(panelWindow, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(panelWindow),
                     MAKELPARAM(panelBounds.left + 5, panelBounds.top + 5));
        host.dispatchCallbacks();
        require(host.callbacks.open == 1, "Status panel did not forward a click to open the app");
        require(host.callbacks.contextMenu == 1, "Status panel did not forward its context menu request");

        panel.update(L"Break 04:59", L"Paused - cycle 1 / 4", true);
        caption = windowCaption(panel.handle());
        require(caption.find(L"Break 04:59") != std::wstring::npos && caption.find(L"Paused") != std::wstring::npos,
                "Status panel did not update the break/paused display");
        SetWindowPos(host.taskList, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        panel.update(L"Timer 00:59", L"Countdown", true);
        require(GetTopWindow(host.window) == panelWindow,
                "Refreshing status did not restore visibility above the task-list container");
        require(windowCaption(panelWindow).find(L"Timer 00:59") != std::wstring::npos,
                "Status panel did not expose countdown text");

        // A visible descendant of a hidden monitor container must not mask the
        // notification-area fallback. Explorer can retain old hidden windows.
        ShowWindow(host.monitorContainer, SW_HIDE);
        panel.update(L"Timer 00:58", L"Countdown", true);
        require(panel.available(), "Status panel lost its notification-area fallback");
        require(screenBounds(panelWindow).right > monitorBefore.left &&
                screenBounds(panelWindow).right <= screenBounds(host.notificationArea).left,
                "Status panel anchored to a hidden Traffic Monitor instead of the notification area");
        ShowWindow(host.monitorContainer, SW_SHOWNOACTIVATE);
        const auto staleMonitor = CreateWindowExW(0, L"STATIC", L"TrafficMonitorTaskbarWindow",
            WS_CHILD | WS_VISIBLE, 1200, 0, 100, 40, host.window, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        require(staleMonitor != nullptr, "Could not create off-taskbar stale monitor");
        panel.update(L"Focus 24:58", L"Cycle 2 / 4", true);
        require(panel.available() && screenBounds(panelWindow).right <= monitorBefore.left,
                "An off-taskbar Traffic Monitor prevented a valid descendant anchor from being used");
        DestroyWindow(staleMonitor);

        // Reproduce the screenshot's adjacent weather widget. No caption or
        // hard-coded weather-class rule is needed to keep compact widgets clear.
        WNDCLASSW weatherType{};
        weatherType.lpfnWndProc = DefWindowProcW;
        weatherType.hInstance = GetModuleHandleW(nullptr);
        weatherType.lpszClassName = L"DynamicContent1";
        require(RegisterClassW(&weatherType) != 0, "Could not register synthetic weather widget");
        HWND weather = CreateWindowExW(0, weatherType.lpszClassName, L"15 C Mostly cloudy", WS_CHILD | WS_VISIBLE,
            650, 0, 140, 46, host.window, nullptr, weatherType.hInstance, nullptr);
        require(weather != nullptr, "Could not create synthetic weather widget");
        const auto weatherBefore = screenBounds(weather);
        panel.update(L"Focus 24:57", L"Cycle 2 / 4", true);
        require(panel.available() && screenBounds(panelWindow).right <= weatherBefore.left,
                "Status panel covered a weather widget to the left of Traffic Monitor");
        requireSameBounds(weather, weatherBefore, "Status panel resized or moved the weather widget");
        ShowWindow(host.monitorContainer, SW_HIDE);
        panel.update(L"Focus 24:57", L"Cycle 2 / 4", true);
        require(panel.available() && screenBounds(panelWindow).right <= weatherBefore.left,
                "Tray fallback covered weather when Traffic Monitor was unavailable");
        ShowWindow(host.monitorContainer, SW_SHOWNOACTIVATE);

        // Traffic Monitor and weather may trade places after Explorer lays out
        // its widgets. Placement follows the left edge of the whole group.
        SetWindowPos(host.monitorContainer, nullptr, 760, 0, 160, 46, SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(weather, nullptr, 925, 0, 30, 46, SWP_NOZORDER | SWP_NOACTIVATE);
        panel.update(L"Focus 24:56", L"Cycle 2 / 4", true);
        require(panel.available() && screenBounds(panelWindow).right <= screenBounds(host.trafficMonitor).left,
                "Reordered widgets moved status over Traffic Monitor");
        SetWindowPos(host.monitorContainer, nullptr, 800, 0, 160, 46, SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(weather, nullptr, 650, 0, 140, 46, SWP_NOZORDER | SWP_NOACTIVATE);
        {
            ForeignTaskbarChild foreign(host.window);
            wchar_t cachedCaption[128]{};
            require(GetWindowTextW(foreign.window, cachedCaption, static_cast<int>(std::size(cachedCaption))) == 0,
                    "Foreign taskbar fixture unexpectedly populated its system caption cache");
            const auto foreignBounds = screenBounds(foreign.window);
            panel.update(L"Focus 24:55", L"Cycle 2 / 4", true);
            require(panel.available() && screenBounds(panelWindow).right <= foreignBounds.left,
                    "Status panel ignored a visible foreign widget with an empty caption cache");
            requireSameBounds(foreign.window, foreignBounds, "Status panel moved the foreign widget");
        }
        panel.update(L"Focus 24:54", L"Cycle 2 / 4", true);
        require(panel.available() && screenBounds(panelWindow).right <= weatherBefore.left &&
                screenBounds(panelWindow).right > hostClient.left + 420,
                "Status panel retained a stale foreign-process widget boundary after it exited");

        // The real overlap can occur when a foreign child is taller than the
        // taskbar and clipped by a full-size Explorer container. Its visible
        // pixels still occupy space; raw-size heuristics must not discard it.
        WNDCLASSW workerType{};
        workerType.lpfnWndProc = DefWindowProcW;
        workerType.hInstance = GetModuleHandleW(nullptr);
        workerType.lpszClassName = L"WorkerW";
        require(RegisterClassW(&workerType) != 0, "Could not register synthetic taskbar container");
        HWND worker = CreateWindowExW(WS_EX_NOPARENTNOTIFY, workerType.lpszClassName, L"", WS_CHILD | WS_VISIBLE,
            0, 0, hostClient.right - hostClient.left, hostClient.bottom - hostClient.top,
            host.window, nullptr, workerType.hInstance, nullptr);
        require(worker != nullptr, "Could not create synthetic taskbar container");
        require(SetPropW(worker, syntheticTaskbarProperty,
            reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(GetCurrentProcessId()))) != FALSE,
            "Could not mark synthetic taskbar container");
        {
            ForeignTaskbarChild clipped(worker);
            require(SetWindowPos(clipped.window, nullptr, 550, -8, 210, 64,
                SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, "Could not resize clipped foreign widget");
            panel.update(L"Timer 00:04", L"Countdown", true);
            const auto clippedBefore = screenBounds(clipped.window);
            require(panel.available() && screenBounds(panelWindow).right + 2 <= clippedBefore.left,
                    "Status panel covered a taller foreign widget clipped by the taskbar");
            requireSameBounds(clipped.window, clippedBefore, "Status panel changed the clipped widget geometry");

            // No full-width slot means hide, rather than cover the neighbor or
            // squeeze the timer into an unreadably narrow rectangle.
            require(SetWindowPos(clipped.window, nullptr, 0, -20, 1200, 90,
                SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, "Could not fill synthetic taskbar with foreign widget");
            panel.update(L"Timer 00:03", L"Countdown", true);
            require(!panel.available() && !IsWindowVisible(panelWindow),
                    "Status panel stayed visible when no collision-free slot existed");
            requireSameBounds(weather, weatherBefore, "No-room handling moved the weather widget");
            require(SetWindowPos(clipped.window, nullptr, 550, -8, 210, 64,
                SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, "Could not restore foreign widget");
            panel.update(L"Timer 00:02", L"Paused", true);
            require(panel.available() && IsWindowVisible(panelWindow),
                    "Status panel did not return after free taskbar space became available");

            // Only the part visible through ancestor clients is occupied.
            require(SetWindowPos(worker, nullptr, 750, 0, 100, hostClient.bottom - hostClient.top,
                SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, "Could not narrow synthetic container");
            require(SetWindowPos(clipped.window, nullptr, -200, -8, 300, 64,
                SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, "Could not clip foreign widget through container");
            panel.update(L"Timer 00:01", L"Countdown", true);
            require(panel.available() && screenBounds(panelWindow).right <= weatherBefore.left &&
                    screenBounds(panelWindow).right > hostClient.left + 550,
                    "Invisible pixels outside the ancestor client area displaced the panel");
        }
        RemovePropW(worker, syntheticTaskbarProperty);
        DestroyWindow(worker);

        // Exercise the asynchronous accessibility path against native buttons
        // in a private task-list window, without reading the real taskbar.
        WNDCLASSW listType{};
        listType.lpfnWndProc = DefWindowProcW;
        listType.hInstance = GetModuleHandleW(nullptr);
        listType.lpszClassName = L"MSTaskListWClass";
        require(RegisterClassW(&listType) != 0, "Could not register private task-list class");
        HWND list = CreateWindowExW(WS_EX_NOPARENTNOTIFY, listType.lpszClassName, L"Private task list",
            WS_CHILD | WS_VISIBLE, 0, 0, 640, 46, host.window, nullptr, listType.hInstance, nullptr);
        require(list != nullptr, "Could not create private task list");
        HWND application = CreateWindowExW(WS_EX_NOPARENTNOTIFY, L"BUTTON", L"Synthetic application",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 300, 46, list, nullptr, listType.hInstance, nullptr);
        require(application != nullptr, "Could not create private application button");
        const auto waitForButtons = [&]() {
            const auto started = GetTickCount64();
            while (GetTickCount64() - started < 5000) {
                auto snapshot = collectTaskbarObstacles(host.window, panel.handle(), false);
                if (snapshot.taskButtonsKnown) return snapshot;
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    require(message.message != WM_QUIT, "Button bounds query unexpectedly quit the test");
                    TranslateMessage(&message); DispatchMessageW(&message);
                }
            }
            throw std::runtime_error("Could not measure private native application-button bounds");
        };
        const auto buttons = waitForButtons();
        require(std::any_of(buttons.occupied.begin(), buttons.occupied.end(), [](const TaskbarRect& rectangle) {
            return rectangle.left <= 2 && rectangle.right >= 298;
        }), "Application button was absent from measured occupied taskbar space");
        panel.update(L"Timer 00:20", L"Countdown", true);
        require(panel.available(), "Measured application buttons left a valid gap but status stayed hidden");

        require(SetWindowPos(application, nullptr, 0, 0, 640, 46,
            SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, "Could not widen private application button");
        require(SetWindowPos(list, nullptr, 0, 0, 641, 46,
            SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, "Could not invalidate private task-list geometry");
        (void)waitForButtons();
        panel.update(L"Timer 00:19", L"Countdown", true);
        require(!panel.available() && !IsWindowVisible(panel.handle()),
                "Status covered an application button when no full-size gap remained");

        require(SetWindowPos(application, nullptr, 0, 0, 300, 46,
            SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, "Could not narrow private application button");
        require(SetWindowPos(list, nullptr, 0, 0, 640, 46,
            SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, "Could not refresh private task-list geometry");
        (void)waitForButtons();
        panel.update(L"Timer 00:18", L"Countdown", true);
        require(panel.available(), "Status did not return after application-button space was freed");
        DestroyWindow(list);
        DestroyWindow(weather);

        panel.update(L"", L"", false);
        require(!IsWindowVisible(panel.handle()), "Disabled or idle status panel remained visible");
        panel.update(L"Focus 24:58", L"Cycle 2 / 4", true);
        require(IsWindowVisible(panel.handle()) != FALSE, "Status panel did not show after being re-enabled");
        panel.hide();
        require(!IsWindowVisible(panel.handle()), "Explicit status-panel hide failed");
        panel.explorerRestarted();
        panel.update(L"Focus 24:58", L"Cycle 2 / 4", true);
        panelWindow = panel.handle();
        require(panel.available() && IsWindowVisible(panelWindow), "Status panel did not rebuild after host refresh");
        require(screenBounds(panelWindow).right <= monitorBefore.left,
                "Rebuilt status panel lost its Traffic Monitor anchor");
        caption = windowCaption(panelWindow);
        require(caption.find(L"Focus 24:58") != std::wstring::npos && caption.find(L"Cycle 2 / 4") != std::wstring::npos,
                "Rebuilt status panel lost unchanged phase/countdown text");
    }
    require(!IsWindow(panelWindow), "Destroying the status panel leaked its native window");
    requireSameBounds(host.window, hostBefore, "Status-panel cleanup changed the taskbar host");
    requireSameBounds(host.taskList, taskListBefore, "Status-panel cleanup changed the task list");
    requireSameBounds(host.trafficMonitor, monitorBefore, "Status-panel cleanup changed Traffic Monitor");
    MSG quit{};
    require(!PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_NOREMOVE),
            "Status-panel cleanup unexpectedly stopped the application message loop");
}

void settingsAndCloseBehavior() {
    RegistrySandbox registry;
    TemporaryData files;
    StateStore store(files.path);
    FakeClock clock({1'700'000'000'000LL, 1'000});
    MainWindow app(GetModuleHandleW(nullptr), store, clock);
    HiddenWindow hidden(app);
    const auto window = app.handle();
    const auto checkbox = GetDlgItem(window, StartupCheck);
    require(checkbox != nullptr, "Startup checkbox is missing");
    const auto statusCheckbox = GetDlgItem(window, StatusCheck);
    require(statusCheckbox != nullptr, "Taskbar status checkbox is missing");
    Button_SetCheck(statusCheckbox, BST_UNCHECKED);
    ComboBox_SetCurSel(GetDlgItem(window, TaskbarCombo), 0);

    const auto saveSettings = [window]() {
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(SaveSettings, BN_CLICKED),
                     reinterpret_cast<LPARAM>(GetDlgItem(window, SaveSettings)));
    };

    // A moved executable leaves a startup value pointing to the old path while
    // the new window's cached matching-path state is false. Explicitly saving
    // Off must remove that stale value even though the checkbox did not change.
    putStartup(L"\"C:\\old-location\\Pomodoro.exe\" --background");
    Button_SetCheck(checkbox, BST_UNCHECKED);
    saveSettings();
    require(!readStartup(), "Saving startup Off did not remove an old executable path");
    require(store.loadPreferences().warning.empty(), "UI preferences did not persist");
    require(!store.loadPreferences().value.showTaskbarStatus, "Taskbar status Off preference did not persist");

    Button_SetCheck(checkbox, BST_CHECKED);
    Button_SetCheck(statusCheckbox, BST_CHECKED);
    saveSettings();
    require(isStartAtSignInEnabled(), "Startup On did not register the current executable");
    const auto expected = readStartup();
    require(expected && expected->find(L"\" --background") != std::wstring::npos,
            "Startup command is not quoted or lacks background launch");

    // The UI now caches On. Reapplying On must repair an externally removed or
    // changed registry value instead of skipping the write based on that cache.
    removeStartup();
    saveSettings();
    require(readStartup() == expected, "Saving unchanged startup On did not repair external removal");
    putStartup(L"\"C:\\old-location\\Pomodoro.exe\" --background");
    saveSettings();
    require(readStartup() == expected, "Saving unchanged startup On did not repair an old executable path");
    require(store.loadPreferences().value.startAtSignIn, "Startup On preference did not persist");
    require(store.loadPreferences().value.showTaskbarStatus, "Taskbar status On preference did not persist");

    Button_SetCheck(checkbox, BST_UNCHECKED);
    saveSettings();
    require(!readStartup(), "Startup Off did not remove this application's value");
    require(!store.loadPreferences().value.startAtSignIn, "Startup Off preference did not persist");
    require(!IsWindowVisible(window), "Settings test unexpectedly showed the hidden window");

    // Drive the real cross-process notification handler without registering a
    // protocol/shortcut or sending anything to the user's notification center.
    const auto click = [window](int id) {
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED),
                     reinterpret_cast<LPARAM>(GetDlgItem(window, id)));
    };
    const auto activate = [window](const std::wstring& uri) {
        COPYDATASTRUCT data{notificationCopyData,
            static_cast<DWORD>((uri.size() + 1) * sizeof(wchar_t)), const_cast<wchar_t*>(uri.c_str())};
        return SendMessageW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data));
    };
    const auto stopUri = [](const wchar_t* timer, const std::string& session) {
        return std::wstring(L"pomodoro-native://stop/") + timer + L"/" + std::wstring(session.begin(), session.end());
    };
    SetDlgItemTextW(window, FocusInput, L"1");
    SetDlgItemTextW(window, BreakInput, L"1");
    SetDlgItemTextW(window, CyclesInput, L"2");
    SetDlgItemTextW(window, HoursInput, L"0");
    SetDlgItemTextW(window, MinutesInput, L"5");
    SetDlgItemTextW(window, SecondsInput, L"0");
    click(PomodoroAction); click(CountdownAction);
    const auto firstSession = store.loadState().value;
    require(firstSession.pomodoro.status == Status::Running && firstSession.countdown.status == Status::Running,
            "Notification action regression did not start both sessions");
    require(activate(L"pomodoro-native://stop/pomodoro/notification-test") == TRUE,
            "Valid test activation was rejected");
    require(store.loadState().value.pomodoro.status == Status::Running,
            "Test notification Stop changed a live session");
    require(activate(L"pomodoro-native://stop/pomodoro/invalid?command=reset") == FALSE,
            "Malformed notification activation was accepted");
    require(activate(stopUri(L"countdown", firstSession.countdown.sessionId)) == TRUE,
            "Countdown notification action was rejected");
    require(store.loadState().value.countdown.status == Status::Idle &&
            store.loadState().value.pomodoro.status == Status::Running,
            "Countdown Stop changed the other timer");
    click(CountdownAction);
    require(activate(stopUri(L"countdown", firstSession.countdown.sessionId)) == TRUE,
            "Stale notification URI was not parsed");
    require(store.loadState().value.countdown.status == Status::Running,
            "A stale notification stopped a new countdown session");
    // Stop at a due phase boundary must suppress the just-completed phase's
    // notice, rather than enqueue a toast for the session being stopped.
    clock.advance(60'000);
    require(activate(stopUri(L"pomodoro", firstSession.pomodoro.sessionId)) == TRUE,
            "Pomodoro notification action was rejected");
    require(store.loadState().value.pomodoro.status == Status::Idle &&
            store.loadState().value.countdown.status == Status::Running,
            "Pomodoro Stop did not reset only its matching session");
    require(!IsWindowVisible(window), "Stop unexpectedly opened the hidden main window");
    click(CountdownReset);

    // Close must terminate the window, including an active session, rather
    // than hiding it in the tray. Its saved UTC deadline remains recoverable.
    SetDlgItemTextW(window, HoursInput, L"0");
    SetDlgItemTextW(window, MinutesInput, L"1");
    SetDlgItemTextW(window, SecondsInput, L"0");
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(CountdownAction, BN_CLICKED),
                 reinterpret_cast<LPARAM>(GetDlgItem(window, CountdownAction)));
    require(store.loadState().value.countdown.status == Status::Running,
            "Countdown did not start before close regression");
    clock.advance(2'000);
    SendMessageW(window, WM_CLOSE, 0, 0);
    require(!IsWindow(window), "Close hid the main window instead of destroying it");
    MSG quit{};
    require(PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) != FALSE,
            "Close did not request termination of the application message loop");
    require(quit.wParam == 0, "Close requested an unexpected process exit status");
    const auto closed = store.loadState();
    require(closed.warning.empty(), "Close did not save a valid timer state");
    require(closed.value.countdown.status == Status::Running, "Close changed the captured countdown state");
    require(closed.value.countdown.deadlineUtcMs - clock.sample.utcMs == 58'000,
            "Close did not preserve the running countdown deadline");
}
} // namespace

int main(int count, char** arguments) {
    try {
        if (count > 1 && std::string(arguments[1]) == "--synthetic-taskbar-child")
            return foreignTaskbarHelper(count, arguments);
        ComLifetime com;
        require(SUCCEEDED(com.status), "Could not initialize COM for native UI test");
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TAB_CLASSES | ICC_STANDARD_CLASSES};
        require(InitCommonControlsEx(&controls) != FALSE, "Could not initialize native controls");
        notificationXmlUsesWindowsRuntimeWithoutSending();
        taskbarStatusUsesOnlyItsSyntheticHost();
        settingsAndCloseBehavior();
        std::cout << "Windows UI taskbar, settings, startup, notification actions, and close regression tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Windows UI regression test failed: " << error.what() << '\n';
        return 1;
    }
}
