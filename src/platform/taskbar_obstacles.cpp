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
    for (HWND current = GetAncestor(window, GA_PARENT); current && current != host;
         current = GetAncestor(current, GA_PARENT)) {
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
    // SetParent does not clear WS_POPUP. Embedded status windows can retain
    // an unrelated hidden owner, which GetParent returns instead of the host.
    for (HWND current = window; current && current != host; current = GetAncestor(current, GA_PARENT)) {
        if (!(GetWindowLongPtrW(current, GWL_STYLE) & WS_VISIBLE) || IsIconic(current) || cloaked(current)) return false;
        HWND parent = GetAncestor(current, GA_PARENT);
        if (!parent) return false;
        RECT client{};
        if (!clientScreenBounds(parent, client) || !IntersectRect(&result, &result, &client)) return false;
    }
    return result.right > result.left && result.bottom > result.top;
}

bool hasVisibleChildContent(HWND window) {
    RECT client{};
    if (!clientScreenBounds(window, client)) return false;
    std::size_t visited = 0;
    for (HWND child = GetWindow(window, GW_CHILD); child && visited < maximumWindows;
         child = GetWindow(child, GW_HWNDNEXT), ++visited) {
        RECT bounds{};
        if (clippedDescendantBounds(child, window, client, bounds)) return true;
    }
    return false;
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
    std::wstring readStatus;
    HWINEVENTHOOK hook = nullptr;
    DWORD hookProcess = 0;
    DWORD hookThread = 0;
    DWORD hookError = ERROR_SUCCESS;
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
        GetAncestor(state.callback, GA_PARENT) == expectedHost)
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
    const DWORD hookError = hook ? ERROR_SUCCESS : GetLastError();
    std::lock_guard<std::mutex> lock(state->mutex);
    state->hook = hook;
    state->hookThread = GetCurrentThreadId();
    state->hookError = hookError;
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

struct AccessibleReadError {
    const wchar_t* stage = L"Accessibility read";
    HRESULT status = E_FAIL;
};

bool readFailure(AccessibleReadError& error, const wchar_t* stage, HRESULT status) {
    error = {stage, FAILED(status) ? status : E_FAIL};
    return false;
}

std::wstring readErrorText(const wchar_t* stage, HRESULT status) {
    wchar_t code[16]{};
    std::swprintf(code, std::size(code), L"0x%08lX", static_cast<unsigned long>(status));
    return std::wstring(stage) + L" failed (" + code + L").";
}

// Per-monitor-aware windows already use physical coordinates. For other
// awareness modes, clip first and transform interior pixel points, never the
// exclusive right/bottom corner outside the host. Expand for integer rounding.
HRESULT physicalBoundsInHost(HWND host, RECT physical, const RECT& logicalClip, RECT& result) {
    const auto user = GetModuleHandleW(L"user32.dll");
    using GetContext = HANDLE (WINAPI*)(HWND);
    using Awareness = int (WINAPI*)(HANDLE);
    const auto context = loadFunction<GetContext>(user, "GetWindowDpiAwarenessContext");
    const auto awareness = loadFunction<Awareness>(user, "GetAwarenessFromDpiAwarenessContext");
    if (context && awareness && awareness(context(host)) == 2 /* DPI_AWARENESS_PER_MONITOR_AWARE */) {
        return IntersectRect(&result, &physical, &logicalClip) ? S_OK : S_FALSE;
    }
    using SetContext = HANDLE (WINAPI*)(HANDLE);
    const auto set = loadFunction<SetContext>(user, "SetThreadDpiAwarenessContext");
    if (!set) return E_NOTIMPL;
    HANDLE previous = set(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)));
    if (!previous) previous = set(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-3)));
    if (!previous) return E_FAIL;
    RECT hostPhysical{};
    const BOOL read = GetWindowRect(host, &hostPhysical);
    set(previous);
    if (!read) return E_FAIL;
    if (!IntersectRect(&physical, &physical, &hostPhysical)) return S_FALSE;
    using Convert = BOOL (WINAPI*)(HWND, LPPOINT);
    const auto convert = loadFunction<Convert>(user, "PhysicalToLogicalPointForPerMonitorDPI");
    if (!convert) return E_NOTIMPL;
    POINT first{physical.left, physical.top}, last{physical.right - 1, physical.bottom - 1};
    if (!convert(host, &first) || !convert(host, &last)) return E_FAIL;
    const auto low = [](LONG coordinate) { return coordinate == LONG_MIN ? LONG_MIN : coordinate - 1; };
    const auto high = [](LONG coordinate) { return coordinate >= LONG_MAX - 1 ? LONG_MAX : coordinate + 2; };
    RECT logical{low((std::min)(first.x, last.x)), low((std::min)(first.y, last.y)),
        high((std::max)(first.x, last.x)), high((std::max)(first.y, last.y))};
    return IntersectRect(&result, &logical, &logicalClip) ? S_OK : S_FALSE;
}

bool appendAccessibleBounds(IAccessible* object, VARIANT child, HWND host,
    const RECT& clip, std::vector<RECT>& occupied, AccessibleReadError& error) {
    LONG x = 0, y = 0, width = 0, height = 0;
    const HRESULT status = object->accLocation(&x, &y, &width, &height, child);
    if (FAILED(status)) return readFailure(error, L"IAccessible::accLocation", status);
    // Empty geometry is a valid result for non-rendered simple elements.
    if (width == 0 || height == 0) return true;
    const std::int64_t right = std::int64_t{x} + width, bottom = std::int64_t{y} + height;
    if (width < 0 || height < 0 || right > LONG_MAX || bottom > LONG_MAX)
        return readFailure(error, L"Accessibility rectangle validation", E_INVALIDARG);
    const RECT physical{x, y, static_cast<LONG>(right), static_cast<LONG>(bottom)};
    RECT bounds{};
    const HRESULT converted = physicalBoundsInHost(host, physical, clip, bounds);
    if (FAILED(converted)) return readFailure(error, L"Accessibility coordinate conversion", converted);
    if (converted == S_OK) occupied.push_back(bounds);
    return true;
}

bool interactiveRole(LONG role) {
    return role == ROLE_SYSTEM_PUSHBUTTON || role == ROLE_SYSTEM_PAGETAB ||
        role == ROLE_SYSTEM_LISTITEM || role == ROLE_SYSTEM_CHECKBUTTON ||
        role == ROLE_SYSTEM_RADIOBUTTON || role == ROLE_SYSTEM_LINK || role == ROLE_SYSTEM_SPLITBUTTON;
}

bool inspectAccessible(IAccessible* object, VARIANT child, HWND host, const RECT& clip,
                       std::vector<RECT>& occupied, std::size_t& remaining,
                       unsigned depth, bool root, AccessibleReadError& error) {
    // Calls are already off the UI thread and limited to one outstanding job.
    // A slow provider must not make every completed measurement unusable;
    // layout generations reject results invalidated while a query was running.
    if (remaining == 0 || depth > 8)
        return readFailure(error, L"Accessibility traversal limits", HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA));
    --remaining;
    // A full child's properties are supplied by its own object, not necessarily
    // its parent. S_FALSE/null is the documented successful simple-element case.
    const bool simple = child.vt == VT_I4 && child.lVal != CHILDID_SELF;
    if (simple) {
        ComPtr<IDispatch> dispatch;
        const HRESULT resolved = object->get_accChild(child, dispatch.put());
        if (SUCCEEDED(resolved) && dispatch.get()) {
            ComPtr<IAccessible> nested;
            const HRESULT queried = dispatch->QueryInterface(IID_IAccessible, reinterpret_cast<void**>(nested.put()));
            if (FAILED(queried))
                return appendAccessibleBounds(object, child, host, clip, occupied, error);
            VARIANT self{}; self.vt = VT_I4; self.lVal = CHILDID_SELF;
            return inspectAccessible(nested.get(), self, host, clip, occupied, remaining, depth + 1, false, error);
        }
        // Even a temporarily unavailable child object can have reliable parent
        // geometry. Cover that rectangle instead of discarding every sibling.
        if (FAILED(resolved)) return appendAccessibleBounds(object, child, host, clip, occupied, error);
    }
    Variant state, role;
    const HRESULT stateStatus = object->get_accState(child, &state.value);
    if (SUCCEEDED(stateStatus) && state.value.vt == VT_I4 &&
        (state.value.lVal & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN))) return true;
    const HRESULT roleStatus = object->get_accRole(child, &role.value);
    if (simple) return appendAccessibleBounds(object, child, host, clip, occupied, error);

    LONG count = 0;
    const HRESULT counted = object->get_accChildCount(&count);
    if (FAILED(counted) || count < 0 || count > static_cast<LONG>(maximumAccessibleNodes)) {
        if (!root) return appendAccessibleBounds(object, child, host, clip, occupied, error);
        return readFailure(error, L"IAccessible::get_accChildCount", FAILED(counted) ? counted : E_INVALIDARG);
    }
    // A full button can contain smaller label/icon objects. Its entire clickable
    // rectangle remains occupied; measuring just its children underestimates it.
    if (!root && SUCCEEDED(roleStatus) && role.value.vt == VT_I4 && interactiveRole(role.value.lVal))
        return appendAccessibleBounds(object, child, host, clip, occupied, error);
    if (count == 0) {
        // The client root is a container even if a proxy labels it as text or
        // graphics. Its full rectangle is never an application-button obstacle.
        if (root) {
            if (FAILED(stateStatus) || FAILED(roleStatus))
                return readFailure(error, L"Accessibility root properties", FAILED(stateStatus) ? stateStatus : roleStatus);
            if (role.value.vt == VT_I4 && (role.value.lVal == ROLE_SYSTEM_PAGETABLIST ||
                role.value.lVal == ROLE_SYSTEM_LIST || role.value.lVal == ROLE_SYSTEM_TOOLBAR)) return true;
            return readFailure(error, L"Accessibility root exposes no button children", E_NOINTERFACE);
        }
        return appendAccessibleBounds(object, child, host, clip, occupied, error);
    }
    std::vector<VARIANT> children(static_cast<std::size_t>(count));
    for (auto& value : children) VariantInit(&value);
    struct ClearChildren {
        std::vector<VARIANT>& values;
        ~ClearChildren() { for (auto& value : values) VariantClear(&value); }
    } clear{children};
    LONG obtained = 0;
    const HRESULT enumerated = AccessibleChildren(object, 0, count, children.data(), &obtained);
    if (FAILED(enumerated) || obtained != count) {
        if (!root) return appendAccessibleBounds(object, child, host, clip, occupied, error);
        return readFailure(error, L"AccessibleChildren", FAILED(enumerated) ? enumerated : E_FAIL);
    }
    for (LONG i = 0; i < obtained; ++i) {
        const auto& value = children[static_cast<std::size_t>(i)];
        bool success = false;
        if (value.vt == VT_DISPATCH && value.pdispVal) {
            ComPtr<IAccessible> nested;
            const HRESULT queried = value.pdispVal->QueryInterface(IID_IAccessible, reinterpret_cast<void**>(nested.put()));
            if (SUCCEEDED(queried) && nested.get()) {
                VARIANT self{}; self.vt = VT_I4; self.lVal = CHILDID_SELF;
                success = inspectAccessible(nested.get(), self, host, clip, occupied, remaining, depth + 1, false, error);
            } else return readFailure(error, L"Child IAccessible QueryInterface", FAILED(queried) ? queried : E_NOINTERFACE);
        } else if (value.vt == VT_I4) {
            success = inspectAccessible(object, value, host, clip, occupied, remaining, depth + 1, false, error);
        } else return readFailure(error, L"AccessibleChildren variant", E_INVALIDARG);
        if (!success) return false;
    }
    return true;
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
    AccessibleReadError error{L"CoInitializeEx for taskbar accessibility", initialized};
    try {
        ThreadDpi dpi(work->job.host);
        success = SUCCEEDED(initialized);
        std::size_t remaining = maximumAccessibleNodes;
        for (const auto& list : work->job.lists) {
            if (!success) break;
            if (!IsWindow(list.window)) {
                success = readFailure(error, L"Task-list window validation", HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE));
                break;
            }
            ComPtr<IAccessible> accessible;
            const HRESULT opened = AccessibleObjectFromWindow(list.window, OBJID_CLIENT, IID_IAccessible,
                reinterpret_cast<void**>(accessible.put()));
            success = SUCCEEDED(opened) && accessible.get();
            if (!success) readFailure(error, L"AccessibleObjectFromWindow", FAILED(opened) ? opened : E_NOINTERFACE);
            VARIANT self{}; self.vt = VT_I4; self.lVal = CHILDID_SELF;
            RECT clipped{};
            if (success && !IntersectRect(&clipped, &list.bounds, &work->job.strip))
                success = readFailure(error, L"Task-list intersection", HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
            if (success) success = inspectAccessible(accessible.get(), self, work->job.host,
                clipped, occupied, remaining, 0, true, error);
        }
    } catch (...) { success = readFailure(error, L"Taskbar accessibility exception", E_FAIL); }
    if (SUCCEEDED(initialized)) CoUninitialize();
    {
        std::lock_guard<std::mutex> lock(work->state->mutex);
        work->state->running = false;
        work->state->success = success && work->generation == work->state->generation;
        work->state->readStatus = work->generation != work->state->generation
            ? L"Taskbar changed during accessibility read; waiting for a fresh measurement."
            : success ? L"" : readErrorText(error.stage, error.status);
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
        if (!snapshot.taskButtonsKnown) {
            if (!state->hook)
                snapshot.buttonStatus = readErrorText(L"SetWinEventHook for taskbar changes",
                    state->hookError ? HRESULT_FROM_WIN32(state->hookError) : E_FAIL);
            else if (state->running)
                snapshot.buttonStatus = L"Taskbar accessibility read is running (" +
                    std::to_wstring(now - state->attempted) + L" ms).";
            else if (!state->readStatus.empty()) snapshot.buttonStatus = state->readStatus;
            else if (state->success && now - state->completed > buttonCacheLifetime)
                snapshot.buttonStatus = L"Taskbar button bounds expired; waiting for a fresh measurement.";
            else if (state->completedGeneration != state->generation)
                snapshot.buttonStatus = L"Taskbar layout generation changed; waiting for button bounds.";
            else snapshot.buttonStatus = L"Waiting for the first taskbar button measurement.";
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
        state->readStatus = readErrorText(L"Starting taskbar accessibility reader", E_OUTOFMEMORY);
        snapshot.buttonStatus = state->readStatus;
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
    if (++search.visited > maximumWindows) {
        search.result.taskButtonsKnown = false;
        search.result.buttonStatus = L"Taskbar child window enumeration exceeded 2048 windows.";
        return FALSE;
    }
    if (window == search.panel || (search.panel && IsChild(search.panel, window))) return TRUE;
    RECT bounds{};
    if (!clippedDescendantBounds(window, search.host, search.strip, bounds)) return TRUE;
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    const auto name = className(window);
    if (process != search.hostProcess) {
        // A known structural host can span the entire strip while only its
        // children draw controls. Do not turn that empty background into a
        // solid obstacle; EnumChildWindows still visits its visible contents.
        // Keep leaf hosts and custom foreign widgets occupied in full.
        if (containerClass(name) && hasVisibleChildContent(window)) return TRUE;
        // Process identity is enough: foreign taskbar children occupy pixels
        // even when their captions, class names, or raw sizes are unexpected.
        addObstacle(search.result, search.host, bounds, true);
        return TRUE;
    }
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
    if (++search.visited > maximumWindows) {
        search.result.taskButtonsKnown = false;
        search.result.buttonStatus = L"Taskbar window enumeration exceeded 2048 windows.";
        return FALSE;
    }
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
        search.result.buttonStatus = L"Could not read the taskbar host client rectangle.";
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
    if ((search.taskContainerSeen || hostClass == L"Shell_TrayWnd") && search.buttons.lists.empty()) {
        search.result.taskButtonsKnown = false;
        search.result.buttonStatus = L"The classic Windows taskbar application-list window was not found.";
    }
    const bool geometryKnown = search.result.taskButtonsKnown;
    const auto geometryStatus = search.result.buttonStatus;
    applicationButtons(search.buttons, panel, search.result);
    search.result.taskButtonsKnown = geometryKnown && search.result.taskButtonsKnown;
    if (!geometryKnown && !geometryStatus.empty()) search.result.buttonStatus = geometryStatus;
    return search.result;
}

} // namespace pomodoro
#endif
