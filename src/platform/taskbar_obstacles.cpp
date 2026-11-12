#include "pomodoro/taskbar_obstacles.h"

#ifdef _WIN32
#include <oleacc.h>
#include <process.h>
#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace pomodoro {
namespace {
constexpr std::size_t maximumWindows = 2048;
constexpr std::size_t maximumAccessibleNodes = 256;
constexpr ULONGLONG buttonCacheLifetime = 1500;
constexpr ULONGLONG buttonQueryInterval = 500;

template<class Function> Function loadFunction(HMODULE module, const char* name) {
    const auto address = module ? GetProcAddress(module, name) : nullptr;
    Function result = nullptr;
    static_assert(sizeof(result) == sizeof(address));
    std::memcpy(&result, &address, sizeof(result));
    return result;
}

class ThreadDpi {
public:
    explicit ThreadDpi(HWND host) {
        using Get = HANDLE (WINAPI*)(HWND);
        const auto user = GetModuleHandleW(L"user32.dll");
        const auto get = loadFunction<Get>(user, "GetWindowDpiAwarenessContext");
        set_ = loadFunction<Set>(user, "SetThreadDpiAwarenessContext");
        if (get && set_) previous_ = set_(get(host));
    }
    ~ThreadDpi() { if (set_ && previous_) set_(previous_); }
private:
    using Set = HANDLE (WINAPI*)(HANDLE);
    Set set_ = nullptr;
    HANDLE previous_ = nullptr;
};

bool cloaked(HWND window) {
    using Get = HRESULT (WINAPI*)(HWND, DWORD, PVOID, DWORD);
    // The system module remains loaded for the lifetime of this process. Only
    // the UI thread calls this function, never the detached COM worker.
    static const auto get = [] {
        const auto module = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return loadFunction<Get>(module, "DwmGetWindowAttribute");
    }();
    DWORD value = 0;
    return get && SUCCEEDED(get(window, 14 /* DWMWA_CLOAKED */, &value, sizeof(value))) && value != 0;
}

std::wstring className(HWND window) {
    wchar_t name[128]{};
    GetClassNameW(window, name, static_cast<int>(std::size(name)));
    return name;
}

bool taskListClass(const std::wstring& name) {
    return name == L"MSTaskListWClass";
}

bool containerClass(const std::wstring& name) {
    return name == L"ReBarWindow32" || name == L"WorkerW" ||
        name == L"MSTaskSwWClass" || taskListClass(name) ||
        name == L"Windows.UI.Composition.DesktopWindowContentBridge";
}

bool underTaskList(HWND window, HWND host) {
    for (HWND current = GetParent(window); current && current != host; current = GetParent(current)) {
        const auto name = className(current);
        if (taskListClass(name) || name == L"MSTaskSwWClass") return true;
    }
    return false;
}

bool clientScreenBounds(HWND window, RECT& result) {
    if (!GetClientRect(window, &result)) return false;
    MapWindowPoints(window, HWND_DESKTOP, reinterpret_cast<POINT*>(&result), 2);
    return result.right > result.left && result.bottom > result.top;
}

bool clippedDescendantBounds(HWND window, HWND host, const RECT& strip, RECT& result) {
    if (!GetWindowRect(window, &result) || !IntersectRect(&result, &result, &strip)) return false;
    for (HWND current = window; current && current != host; current = GetParent(current)) {
        if (!(GetWindowLongPtrW(current, GWL_STYLE) & WS_VISIBLE) || IsIconic(current) || cloaked(current)) return false;
        HWND parent = GetParent(current);
        if (!parent) return false;
        RECT client{};
        if (!clientScreenBounds(parent, client) || !IntersectRect(&result, &result, &client)) return false;
    }
    return result.right > result.left && result.bottom > result.top;
}

void addObstacle(TaskbarObstacleSnapshot& snapshot, HWND host, RECT screen, bool widget) {
    MapWindowPoints(HWND_DESKTOP, host, reinterpret_cast<POINT*>(&screen), 2);
    const TaskbarRect value{static_cast<int>(screen.left), static_cast<int>(screen.top),
        static_cast<int>(screen.right), static_cast<int>(screen.bottom)};
    snapshot.occupied.push_back(value);
    if (widget && (!snapshot.widgetLeft || value.left < *snapshot.widgetLeft)) snapshot.widgetLeft = value.left;
}

struct TaskListSignature {
    HWND window;
    RECT bounds;
};

struct ButtonJob {
    HWND host = nullptr;
    DWORD process = 0;
    RECT strip{};
    std::vector<TaskListSignature> lists;
};

bool sameJob(const ButtonJob& a, const ButtonJob& b) {
    if (a.host != b.host || a.process != b.process || !EqualRect(&a.strip, &b.strip) || a.lists.size() != b.lists.size()) return false;
    for (std::size_t i = 0; i < a.lists.size(); ++i)
        if (a.lists[i].window != b.lists[i].window || !EqualRect(&a.lists[i].bounds, &b.lists[i].bounds)) return false;
    return true;
}

struct ButtonState {
    ~ButtonState() {
        if (hook && hookThread == GetCurrentThreadId()) UnhookWinEvent(hook);
    }
    std::mutex mutex;
    bool running = false;
    bool success = false;
    ULONGLONG attempted = 0;
    ULONGLONG completed = 0;
    HWND callback = nullptr;
    ButtonJob attemptedJob;
    ButtonJob completedJob;
    std::vector<RECT> occupied;
    HWINEVENTHOOK hook = nullptr;
    DWORD hookProcess = 0;
    DWORD hookThread = 0;
    std::vector<HWND> watchedLists;
    std::vector<HWND> watchedWindows;
    std::uint64_t generation = 1;
    std::uint64_t attemptedGeneration = 0;
    std::uint64_t completedGeneration = 0;
};

std::shared_ptr<ButtonState> buttonState() {
    static const auto value = std::make_shared<ButtonState>();
    return value;
}

// Caller holds the state mutex. Never send a private message to a recycled
// HWND belonging to another window or process.
void postButtonsReady(ButtonState& state, HWND expectedHost) {
    DWORD process = 0;
    wchar_t name[128]{};
    GetWindowThreadProcessId(state.callback, &process);
    GetClassNameW(state.callback, name, static_cast<int>(std::size(name)));
    if (process == GetCurrentProcessId() &&
        std::wcscmp(name, L"PomodoroNative.TaskbarStatus.v1") == 0 &&
        GetParent(state.callback) == expectedHost)
        PostMessageW(state.callback, taskbarObstaclesReadyMessage, 0, 0);
}

void CALLBACK buttonLayoutChanged(HWINEVENTHOOK hook, DWORD event, HWND window,
                                  LONG, LONG, DWORD, DWORD) {
    if (event != EVENT_OBJECT_CREATE && event != EVENT_OBJECT_DESTROY &&
        event != EVENT_OBJECT_SHOW && event != EVENT_OBJECT_HIDE &&
        event != EVENT_OBJECT_REORDER && event != EVENT_OBJECT_LOCATIONCHANGE) return;
    try {
        const auto state = buttonState();
        std::lock_guard<std::mutex> lock(state->mutex);
        if (hook != state->hook || !window) return;
        bool relevant = std::find(state->watchedWindows.begin(), state->watchedWindows.end(), window) !=
            state->watchedWindows.end();
        for (const auto list : state->watchedLists) {
            if (relevant || window == list || IsChild(list, window)) { relevant = true; break; }
        }
        if (!relevant) return;
        ++state->generation;
        state->success = false;
        postButtonsReady(*state, state->attemptedJob.host);
    } catch (...) {
        // WinEvent callbacks must never propagate a C++ exception into USER32.
    }
}

BOOL CALLBACK rememberTaskChild(HWND window, LPARAM context) {
    auto& children = *reinterpret_cast<std::vector<HWND>*>(context);
    if (children.size() >= maximumWindows) return FALSE;
    children.push_back(window);
    return TRUE;
}

void watchTaskLists(const std::shared_ptr<ButtonState>& state, const ButtonJob& job, HWND panel) {
    std::vector<HWND> lists, windows;
    for (const auto& list : job.lists) {
        lists.push_back(list.window);
        windows.push_back(list.window);
        EnumChildWindows(list.window, rememberTaskChild, reinterpret_cast<LPARAM>(&windows));
    }
    const DWORD process = lists.empty() ? 0 : job.process;
    HWINEVENTHOOK retired = nullptr;
    bool install = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->callback = panel;
        if (state->hookProcess != process || state->watchedLists != lists) {
            retired = state->hook;
            state->hook = nullptr;
            state->hookProcess = process;
            state->watchedLists = std::move(lists);
            ++state->generation;
            state->success = false;
        }
        state->watchedWindows = std::move(windows);
        install = process != 0 && !state->hook;
    }
    if (retired) UnhookWinEvent(retired);
    if (!install) return;
    const DWORD flags = WINEVENT_OUTOFCONTEXT |
        (process == GetCurrentProcessId() ? 0u : WINEVENT_SKIPOWNPROCESS);
    const auto hook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_LOCATIONCHANGE,
        nullptr, buttonLayoutChanged, process, 0, flags);
    std::lock_guard<std::mutex> lock(state->mutex);
    state->hook = hook;
    state->hookThread = GetCurrentThreadId();
}

template<class T> class ComPtr {
public:
    ~ComPtr() { if (value_) value_->Release(); }
    T* operator->() const { return value_; }
    T* get() const { return value_; }
    T** put() { return &value_; }
private:
    T* value_ = nullptr;
};

class Variant {
public:
    Variant() { VariantInit(&value); }
    ~Variant() { VariantClear(&value); }
    VARIANT value;
};

bool occupiedRole(LONG role) {
    return role == ROLE_SYSTEM_PUSHBUTTON || role == ROLE_SYSTEM_PAGETAB ||
        role == ROLE_SYSTEM_LISTITEM || role == ROLE_SYSTEM_CHECKBUTTON ||
        role == ROLE_SYSTEM_RADIOBUTTON || role == ROLE_SYSTEM_LINK || role == ROLE_SYSTEM_SPLITBUTTON ||
        role == ROLE_SYSTEM_STATICTEXT || role == ROLE_SYSTEM_SEPARATOR ||
        role == ROLE_SYSTEM_GRAPHIC || role == ROLE_SYSTEM_GRIP;
}

bool inspectAccessible(IAccessible* object, VARIANT child, HWND host, const RECT& clip,
                       std::vector<RECT>& occupied, std::size_t& remaining,
                       ULONGLONG started, unsigned depth, bool root) {
    if (remaining == 0 || depth > 8 || GetTickCount64() - started > 500) return false;
    --remaining;
    Variant state, role;
    if (FAILED(object->get_accState(child, &state.value)) || FAILED(object->get_accRole(child, &role.value))) return false;
    if (state.value.vt == VT_I4 && (state.value.lVal & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN))) return true;
    if (role.value.vt == VT_I4 && occupiedRole(role.value.lVal)) {
        LONG x = 0, y = 0, width = 0, height = 0;
        if (FAILED(object->accLocation(&x, &y, &width, &height, child))) return false;
        const std::int64_t right = std::int64_t{x} + width, bottom = std::int64_t{y} + height;
        if (width <= 0 || height <= 0 || right > LONG_MAX || bottom > LONG_MAX) return false;
        // accLocation returns physical pixels; Win32 geometry above follows
        // the host's awareness. Convert explicitly for system/unaware hosts.
        using Convert = BOOL (WINAPI*)(HWND, LPPOINT);
        static const auto convert = loadFunction<Convert>(GetModuleHandleW(L"user32.dll"),
            "PhysicalToLogicalPointForPerMonitorDPI");
        POINT first{x, y}, last{static_cast<LONG>(right), static_cast<LONG>(bottom)};
        if (!convert || !convert(host, &first) || !convert(host, &last)) return false;
        RECT bounds{first.x, first.y, last.x, last.y};
        if (IntersectRect(&bounds, &bounds, &clip)) occupied.push_back(bounds);
        return true;
    }
    if (child.vt == VT_I4 && child.lVal != CHILDID_SELF) {
        ComPtr<IDispatch> dispatch;
        if (FAILED(object->get_accChild(child, dispatch.put())) || !dispatch.get()) return false;
        ComPtr<IAccessible> nested;
        if (FAILED(dispatch->QueryInterface(IID_IAccessible, reinterpret_cast<void**>(nested.put())))) return false;
        VARIANT self{}; self.vt = VT_I4; self.lVal = CHILDID_SELF;
        return inspectAccessible(nested.get(), self, host, clip, occupied, remaining, started, depth + 1, false);
    }
    LONG count = 0;
    if (FAILED(object->get_accChildCount(&count)) || count < 0 || count > static_cast<LONG>(maximumAccessibleNodes)) return false;
    if (count == 0) {
        // An empty list is meaningful; an opaque root proxy is not evidence
        // that its application-button area is empty.
        return !root || (role.value.vt == VT_I4 &&
            (role.value.lVal == ROLE_SYSTEM_PAGETABLIST || role.value.lVal == ROLE_SYSTEM_LIST));
    }
    std::vector<VARIANT> children(static_cast<std::size_t>(count));
    for (auto& value : children) VariantInit(&value);
    struct ClearChildren {
        std::vector<VARIANT>& values;
        ~ClearChildren() { for (auto& value : values) VariantClear(&value); }
    } clear{children};
    LONG obtained = 0;
    const HRESULT status = AccessibleChildren(object, 0, count, children.data(), &obtained);
    bool success = SUCCEEDED(status) && obtained == count;
    for (LONG i = 0; success && i < obtained; ++i) {
        const auto& value = children[static_cast<std::size_t>(i)];
        if (value.vt == VT_DISPATCH && value.pdispVal) {
            ComPtr<IAccessible> nested;
            success = SUCCEEDED(value.pdispVal->QueryInterface(IID_IAccessible, reinterpret_cast<void**>(nested.put())));
            VARIANT self{}; self.vt = VT_I4; self.lVal = CHILDID_SELF;
            if (success) success = inspectAccessible(nested.get(), self, host, clip, occupied, remaining, started, depth + 1, false);
        } else if (value.vt == VT_I4) {
            success = inspectAccessible(object, value, host, clip, occupied, remaining, started, depth + 1, false);
        } else success = false;
    }
    return success;
}

struct WorkerArguments {
    std::shared_ptr<ButtonState> state;
    ButtonJob job;
    std::uint64_t generation = 0;
};

unsigned __stdcall readButtons(void* parameter) {
    std::unique_ptr<WorkerArguments> work(static_cast<WorkerArguments*>(parameter));
    bool success = false;
    std::vector<RECT> occupied;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        ThreadDpi dpi(work->job.host);
        success = SUCCEEDED(initialized);
        const auto started = GetTickCount64();
        std::size_t remaining = maximumAccessibleNodes;
        for (const auto& list : work->job.lists) {
            if (!success || !IsWindow(list.window)) { success = false; break; }
            ComPtr<IAccessible> accessible;
            success = SUCCEEDED(AccessibleObjectFromWindow(list.window, OBJID_CLIENT, IID_IAccessible,
                reinterpret_cast<void**>(accessible.put())));
            VARIANT self{}; self.vt = VT_I4; self.lVal = CHILDID_SELF;
            RECT clipped{};
            if (success) success = IntersectRect(&clipped, &list.bounds, &work->job.strip) != FALSE &&
                inspectAccessible(accessible.get(), self, work->job.host, clipped, occupied, remaining, started, 0, true);
        }
    } catch (...) { success = false; }
    if (SUCCEEDED(initialized)) CoUninitialize();
    {
        std::lock_guard<std::mutex> lock(work->state->mutex);
        work->state->running = false;
        work->state->success = success && work->generation == work->state->generation;
        work->state->completed = GetTickCount64();
        work->state->completedJob = std::move(work->job);
        work->state->completedGeneration = work->generation;
        work->state->occupied = std::move(occupied);
        postButtonsReady(*work->state, work->state->completedJob.host);
    }
    return 0;
}

void applicationButtons(const ButtonJob& job, HWND panel, TaskbarObstacleSnapshot& snapshot) {
    const auto state = buttonState();
    watchTaskLists(state, job, panel);
    if (job.lists.empty()) return;
    const auto now = GetTickCount64();
    std::unique_ptr<WorkerArguments> work;
    snapshot.taskButtonsKnown = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->hook && state->success && state->completedGeneration == state->generation &&
            now - state->completed <= buttonCacheLifetime && sameJob(job, state->completedJob)) {
            snapshot.taskButtonsKnown = true;
            for (const auto& bounds : state->occupied) addObstacle(snapshot, job.host, bounds, false);
        }
        if (state->hook && !state->running && (!sameJob(job, state->attemptedJob) ||
            state->attemptedGeneration != state->generation || now - state->attempted >= buttonQueryInterval)) {
            work = std::make_unique<WorkerArguments>();
            work->state = state;
            work->job = job;
            work->generation = state->generation;
            state->attemptedJob = job;
            state->attemptedGeneration = state->generation;
            state->attempted = now;
            state->running = true;
        }
    }
    if (!work) return;
    HANDLE thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, readButtons, work.get(), 0, nullptr));
    if (thread) { work.release(); CloseHandle(thread); }
    else {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->running = false;
    }
}

struct WindowSearch {
    HWND host;
    HWND panel;
    DWORD hostProcess;
    RECT strip;
    TaskbarObstacleSnapshot result;
    ButtonJob buttons;
    bool taskContainerSeen = false;
    std::size_t visited = 0;
};

BOOL CALLBACK descendant(HWND window, LPARAM context) {
    auto& search = *reinterpret_cast<WindowSearch*>(context);
    if (++search.visited > maximumWindows) { search.result.taskButtonsKnown = false; return FALSE; }
    if (window == search.panel || (search.panel && IsChild(search.panel, window))) return TRUE;
    RECT bounds{};
    if (!clippedDescendantBounds(window, search.host, search.strip, bounds)) return TRUE;
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != search.hostProcess) {
        // Process identity is enough: foreign taskbar children occupy pixels
        // even when their captions, class names, or raw sizes are unexpected.
        addObstacle(search.result, search.host, bounds, true);
        return TRUE;
    }
    const auto name = className(window);
    if (name == L"MSTaskSwWClass" || taskListClass(name)) search.taskContainerSeen = true;
    if (taskListClass(name)) {
        search.buttons.lists.push_back({window, bounds});
        return TRUE;
    }
    if (containerClass(name) || name == L"tooltips_class32") return TRUE;
    const bool taskChild = underTaskList(window, search.host);
    const LONG width = search.strip.right - search.strip.left;
    const bool compact = bounds.right - bounds.left <= width / 2;
    const bool rightSide = bounds.left >= search.strip.left + width / 2;
    const bool namedWidget = name == L"TrayNotifyWnd" || name == L"ToolbarWindow32" ||
        name.compare(0, 14, L"DynamicContent") == 0 || name == L"TrayButton" ||
        name == L"TrayShowDesktopButtonWClass";
    if (namedWidget || (compact && rightSide && !taskChild)) {
        addObstacle(search.result, search.host, bounds,
            !taskChild && (rightSide || name == L"TrayNotifyWnd"));
    } else if (compact && GetWindow(window, GW_CHILD) == nullptr) {
        // Small native leaves, including controls inside the task list, are
        // occupied too. They do not move the preferred widget boundary left.
        addObstacle(search.result, search.host, bounds, false);
    }
    return TRUE;
}

BOOL CALLBACK aboveTaskbar(HWND window, LPARAM context) {
    auto& search = *reinterpret_cast<WindowSearch*>(context);
    if (window == search.host) return FALSE;
    if (++search.visited > maximumWindows) { search.result.taskButtonsKnown = false; return FALSE; }
    if (!(GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) || IsIconic(window) || cloaked(window)) return TRUE;
    RECT bounds{};
    if (GetWindowRect(window, &bounds) && IntersectRect(&bounds, &bounds, &search.strip))
        addObstacle(search.result, search.host, bounds, true);
    return TRUE;
}
} // namespace

TaskbarObstacleSnapshot collectTaskbarObstacles(HWND host, HWND panel, bool includeTopLevel) {
    WindowSearch search{};
    search.host = host;
    search.panel = panel;
    if (!host || !clientScreenBounds(host, search.strip)) {
        search.result.taskButtonsKnown = false;
        return search.result;
    }
    GetWindowThreadProcessId(host, &search.hostProcess);
    search.buttons.host = host;
    search.buttons.process = search.hostProcess;
    search.buttons.strip = search.strip;
    EnumChildWindows(host, descendant, reinterpret_cast<LPARAM>(&search));
    // Top-level enumeration is in z-order. Windows behind the taskbar cannot
    // occlude it, even when invisible resize borders intersect its strip.
    if (includeTopLevel) EnumWindows(aboveTaskbar, reinterpret_cast<LPARAM>(&search));
    const auto hostClass = className(host);
    if ((search.taskContainerSeen || hostClass == L"Shell_TrayWnd") && search.buttons.lists.empty())
        search.result.taskButtonsKnown = false;
    const bool geometryKnown = search.result.taskButtonsKnown;
    applicationButtons(search.buttons, panel, search.result);
    search.result.taskButtonsKnown = geometryKnown && search.result.taskButtonsKnown;
    return search.result;
}

} // namespace pomodoro
#endif
