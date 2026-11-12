#include "pomodoro/windows_notifications.h"
#include "pomodoro/notification_request.h"

#ifdef _WIN32
#include <windows.h>
#include <roapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propsys.h>
#include <cstring>
#include <atomic>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

namespace pomodoro {
namespace {
constexpr wchar_t appId[] = L"PomodoroNative.Desktop";
constexpr wchar_t group[] = L"timers";
constexpr wchar_t managerClass[] = L"Windows.UI.Notifications.ToastNotificationManager";
constexpr wchar_t toastClass[] = L"Windows.UI.Notifications.ToastNotification";
constexpr wchar_t documentClass[] = L"Windows.Data.Xml.Dom.XmlDocument";
constexpr GUID stubClsid = {0x83edb410,0xf73c,0x4e5b,{0x9b,0x72,0x04,0xb2,0xd1,0xa5,0xd9,0x31}};
constexpr wchar_t stubClsidText[] = L"{83EDB410-F73C-4E5B-9B72-04B2D1A5D931}";
constexpr PROPERTYKEY appIdKey = {{0x9f4c2855,0x9f79,0x4b39,{0xa8,0xd0,0xe1,0xd4,0x2d,0xe1,0xd5,0xf3}},5};
constexpr PROPERTYKEY activatorKey = {{0x9f4c2855,0x9f79,0x4b39,{0xa8,0xd0,0xe1,0xd4,0x2d,0xe1,0xd5,0xf3}},26};

// Minimal ABI prefixes, verified against Microsoft's Windows SDK definitions:
// microsoft/win32metadata generation/WinSDK/RecompiledIdlHeaders/winrt/
// windows.ui.notifications.h and windows.data.xml.dom.h. These declarations
// also support the older MinGW SDK, which omits the notification headers.
constexpr IID managerId = {0x50ac103f,0xd235,0x4598,{0xbb,0xef,0x98,0xfe,0x4d,0x1a,0x3a,0xd4}};
constexpr IID manager2Id = {0x7ab93c52,0x0e48,0x4750,{0xba,0x9d,0x1a,0x41,0x13,0x98,0x18,0x47}};
constexpr IID toastFactoryId = {0x04124b20,0x82c6,0x4229,{0xb1,0x09,0xfd,0x9e,0xd4,0x66,0x2b,0x53}};
constexpr IID toast2Id = {0x9dfb9fd1,0x143a,0x490e,{0x90,0xbf,0xb9,0xfb,0xa7,0x13,0x2d,0xe7}};
constexpr IID documentId = {0xf7f3a506,0x1e87,0x42d6,{0xbc,0xfb,0xb8,0xc8,0x09,0xfa,0x54,0x94}};
constexpr IID documentIoId = {0x6cd0e74e,0xee65,0x4489,{0x9e,0xbf,0xca,0x43,0xe8,0x7b,0xa6,0x37}};
constexpr IID activationFactoryId = {0x00000035,0x0000,0x0000,{0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
constexpr IID toastId = {0x997e2675,0x059e,0x4e60,{0x8b,0x06,0x17,0x60,0x91,0x7c,0x8b,0x80}};
constexpr IID failedHandlerId = {0x95e3e803,0xc969,0x5e3a,{0x97,0x53,0xea,0x2a,0xd2,0x2a,0x9a,0x33}};
// IAgileObject is a marker interface with only IUnknown's methods.
constexpr IID agileId = {0x94ea2b94,0xe9cc,0x49e0,{0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90}};

struct Notifier : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE Show(IInspectable*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Hide(IInspectable*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Setting(int*) = 0;
};
struct Manager : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE CreateToastNotifier(Notifier**) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateToastNotifierWithId(HSTRING, Notifier**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTemplateContent(int, IInspectable**) = 0;
};
struct History : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RemoveGroup(HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveGroupWithId(HSTRING, HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveGroupedTagWithId(HSTRING, HSTRING, HSTRING) = 0;
};
struct Manager2 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_History(History**) = 0;
};
struct ToastFactory : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE CreateToastNotification(IInspectable*, IInspectable**) = 0;
};
struct Toast2 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Tag(HSTRING) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Tag(HSTRING*) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Group(HSTRING) = 0;
};
struct DocumentIo : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE LoadXml(HSTRING) = 0;
};
struct EventToken { INT64 value; };
struct FailedArgs : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT*) = 0;
};
struct ToastEvents : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Content(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_ExpirationTime(IInspectable*) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ExpirationTime(IInspectable**) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_Dismissed(IUnknown*, EventToken*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_Dismissed(EventToken) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_Activated(IUnknown*, EventToken*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_Activated(EventToken) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_Failed(IUnknown*, EventToken*) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_Failed(EventToken) = 0;
};

template<class T> class ComPtr {
public:
    ~ComPtr() { reset(); }
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T* get() const noexcept { return value_; }
    T* operator->() const noexcept { return value_; }
    T** put() { reset(); return &value_; }
    void reset() { if (value_) value_->Release(); value_ = nullptr; }
private:
    T* value_ = nullptr;
};

template<class Function> Function function(HMODULE module, const char* name) {
    const auto address = GetProcAddress(module, name);
    Function result = nullptr;
    static_assert(sizeof(address) == sizeof(result));
    std::memcpy(&result, &address, sizeof(result));
    return result;
}

std::wstring errorText(const wchar_t* operation, HRESULT status) {
    wchar_t* buffer = nullptr;
    const DWORD count = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        static_cast<DWORD>(status), 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wostringstream message;
    message << operation << L" (0x" << std::hex << std::uppercase << std::setw(8)
            << std::setfill(L'0') << static_cast<unsigned long>(status) << L")";
    if (count) {
        std::wstring detail(buffer, count);
        while (!detail.empty() && (detail.back() == L'\r' || detail.back() == L'\n' || detail.back() == L' ')) detail.pop_back();
        message << L": " << detail;
    }
    if (buffer) LocalFree(buffer);
    return message.str();
}

struct FailureTicket { bool active = true; };
struct FailureState {
    std::mutex mutex;
    HWND target = nullptr;
    UINT message = 0;
    std::wstring error;
    std::weak_ptr<FailureTicket> errorTicket;
};

// This delegate touches only immutable shared_ptrs and mutex-protected state,
// so it can safely expose IAgileObject and run on a notification worker thread.
struct FailedHandler final : IUnknown {
    FailedHandler(std::shared_ptr<FailureState> state, std::shared_ptr<FailureTicket> ticket)
        : state_(std::move(state)), ticket_(std::move(ticket)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, failedHandlerId) || IsEqualIID(iid, agileId)) {
            *result = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
    virtual HRESULT STDMETHODCALLTYPE Invoke(ToastEvents*, FailedArgs* args) {
        try {
            HRESULT failure = E_FAIL;
            if (args) {
                const HRESULT status = args->get_ErrorCode(&failure);
                if (FAILED(status)) failure = status;
            }
            const auto message = errorText(L"Windows failed to deliver the notification", failure);
            std::lock_guard<std::mutex> lock(state_->mutex);
            if (!ticket_->active) return S_OK;
            state_->error = message;
            state_->errorTicket = ticket_;
            if (state_->target && state_->message) PostMessageW(state_->target, state_->message, 0, 0);
            return S_OK;
        } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
        catch (...) { return E_FAIL; }
    }
private:
    std::atomic<ULONG> references_{1};
    const std::shared_ptr<FailureState> state_;
    const std::shared_ptr<FailureTicket> ticket_;
};

struct Subscription {
    Subscription(std::shared_ptr<FailureState> failureState) : state(std::move(failureState)) {}
    ~Subscription() {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            ticket->active = false;
        }
        if (connected) toast->remove_Failed(token);
    }
    HRESULT connect(IInspectable* object) {
        HRESULT status = object->QueryInterface(toastId, reinterpret_cast<void**>(toast.put()));
        if (FAILED(status)) return status;
        auto* handler = new FailedHandler(state, ticket);
        status = toast->add_Failed(handler, &token);
        handler->Release();
        connected = SUCCEEDED(status);
        return status;
    }
    std::shared_ptr<FailureState> state;
    std::shared_ptr<FailureTicket> ticket = std::make_shared<FailureTicket>();
    ComPtr<ToastEvents> toast;
    EventToken token{};
    bool connected = false;
};

class Runtime {
public:
    using Initialize = HRESULT (WINAPI*)(RO_INIT_TYPE);
    using Uninitialize = void (WINAPI*)();
    using CreateString = HRESULT (WINAPI*)(PCNZWCH, UINT32, HSTRING*);
    using DeleteString = HRESULT (WINAPI*)(HSTRING);
    using GetFactory = HRESULT (WINAPI*)(HSTRING, REFIID, void**);
    ~Runtime() {
        if (initialized_) uninitialize_();
        if (module_) FreeLibrary(module_);
    }
    HRESULT initialize() {
        if (initialized_) return S_OK;
        if (!module_) module_ = LoadLibraryExW(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_) return HRESULT_FROM_WIN32(GetLastError());
        auto initialize = function<Initialize>(module_, "RoInitialize");
        uninitialize_ = function<Uninitialize>(module_, "RoUninitialize");
        createString = function<CreateString>(module_, "WindowsCreateString");
        deleteString = function<DeleteString>(module_, "WindowsDeleteString");
        getFactory = function<GetFactory>(module_, "RoGetActivationFactory");
        if (!initialize || !uninitialize_ || !createString || !deleteString || !getFactory) return E_NOTIMPL;
        const HRESULT status = initialize(RO_INIT_SINGLETHREADED);
        initialized_ = SUCCEEDED(status);
        return status;
    }
    CreateString createString = nullptr;
    DeleteString deleteString = nullptr;
    GetFactory getFactory = nullptr;
private:
    HMODULE module_ = nullptr;
    Uninitialize uninitialize_ = nullptr;
    bool initialized_ = false;
};

class String {
public:
    explicit String(Runtime& runtime) : runtime_(runtime) {}
    ~String() { if (value_) runtime_.deleteString(value_); }
    HRESULT set(const std::wstring& value) {
        if (value.size() > (std::numeric_limits<UINT32>::max)()) return E_INVALIDARG;
        return runtime_.createString(value.data(), static_cast<UINT32>(value.size()), &value_);
    }
    HSTRING get() const noexcept { return value_; }
private:
    Runtime& runtime_;
    HSTRING value_ = nullptr;
};

template<class T> HRESULT factory(Runtime& runtime, const wchar_t* name,
                                 REFIID iid, ComPtr<T>& target) {
    String className(runtime);
    HRESULT status = className.set(name);
    if (FAILED(status)) return status;
    return runtime.getFactory(className.get(), iid, reinterpret_cast<void**>(target.put()));
}

HRESULT createToast(Runtime& runtime, const std::wstring& xml,
                    const std::wstring& tag, ComPtr<IInspectable>& toast) {
    ComPtr<IActivationFactory> documentFactory;
    HRESULT status = factory(runtime, documentClass, activationFactoryId, documentFactory);
    if (FAILED(status)) return status;
    ComPtr<IInspectable> documentObject;
    status = documentFactory->ActivateInstance(documentObject.put());
    if (FAILED(status)) return status;
    ComPtr<DocumentIo> io;
    status = documentObject->QueryInterface(documentIoId, reinterpret_cast<void**>(io.put()));
    if (FAILED(status)) return status;
    String content(runtime);
    status = content.set(xml);
    if (FAILED(status)) return status;
    status = io->LoadXml(content.get());
    if (FAILED(status)) return status;
    ComPtr<IInspectable> document;
    status = documentObject->QueryInterface(documentId, reinterpret_cast<void**>(document.put()));
    if (FAILED(status)) return status;
    ComPtr<ToastFactory> creator;
    status = factory(runtime, toastClass, toastFactoryId, creator);
    if (FAILED(status)) return status;
    status = creator->CreateToastNotification(document.get(), toast.put());
    if (FAILED(status)) return status;
    ComPtr<Toast2> properties;
    status = toast->QueryInterface(toast2Id, reinterpret_cast<void**>(properties.put()));
    if (FAILED(status)) return status;
    String tagString(runtime), groupString(runtime);
    status = tagString.set(tag);
    if (SUCCEEDED(status)) status = groupString.set(group);
    if (SUCCEEDED(status)) status = properties->put_Tag(tagString.get());
    if (SUCCEEDED(status)) status = properties->put_Group(groupString.get());
    return status;
}

HRESULT executablePath(std::wstring& result) {
    std::vector<wchar_t> buffer(512);
    while (buffer.size() <= 32768) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!length) return HRESULT_FROM_WIN32(GetLastError());
        if (length < buffer.size()) { result.assign(buffer.data(), length); return S_OK; }
        buffer.resize(buffer.size() * 2);
    }
    return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
}

HRESULT registryString(const wchar_t* path, const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
    status = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return HRESULT_FROM_WIN32(status);
}

HRESULT registerIdentity() {
    std::wstring executable;
    HRESULT status = executablePath(executable);
    if (FAILED(status)) return status;
    status = registryString(L"Software\\Classes\\pomodoro-native", nullptr, L"URL:Pomodoro notification action");
    if (SUCCEEDED(status)) status = registryString(L"Software\\Classes\\pomodoro-native", L"URL Protocol", L"");
    if (SUCCEEDED(status)) status = registryString(L"Software\\Classes\\pomodoro-native\\shell\\open\\command", nullptr,
        L"\"" + executable + L"\" --notification \"%1\"");
    if (SUCCEEDED(status)) status = registryString(L"Software\\Classes\\AppUserModelId\\PomodoroNative.Desktop", L"DisplayName", L"Pomodoro");
    if (SUCCEEDED(status)) status = registryString(L"Software\\Classes\\AppUserModelId\\PomodoroNative.Desktop", L"CustomActivator", stubClsidText);
    if (FAILED(status)) return status;

    PWSTR programs = nullptr;
    status = SHGetKnownFolderPath(FOLDERID_Programs, KF_FLAG_CREATE, nullptr, &programs);
    if (FAILED(status)) return status;
    const std::wstring shortcut = std::wstring(programs) + L"\\Pomodoro Notifications.lnk";
    CoTaskMemFree(programs);
    ComPtr<IShellLinkW> link;
    status = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
        IID_IShellLinkW, reinterpret_cast<void**>(link.put()));
    if (SUCCEEDED(status)) status = link->SetPath(executable.c_str());
    if (SUCCEEDED(status)) status = link->SetDescription(L"Pomodoro focus, break and timer notifications");
    if (SUCCEEDED(status)) status = link->SetWorkingDirectory(executable.substr(0, executable.find_last_of(L"\\/")).c_str());
    if (SUCCEEDED(status)) status = link->SetIconLocation(executable.c_str(), 0);
    ComPtr<IPropertyStore> properties;
    if (SUCCEEDED(status)) status = link->QueryInterface(IID_IPropertyStore, reinterpret_cast<void**>(properties.put()));
    PROPVARIANT id{};
    id.vt = VT_LPWSTR;
    id.pwszVal = const_cast<PWSTR>(appId);
    if (SUCCEEDED(status)) status = properties->SetValue(appIdKey, id);
    PROPVARIANT activator{};
    activator.vt = VT_CLSID;
    activator.puuid = const_cast<GUID*>(&stubClsid);
    if (SUCCEEDED(status)) status = properties->SetValue(activatorKey, activator);
    if (SUCCEEDED(status)) status = properties->Commit();
    ComPtr<IPersistFile> file;
    if (SUCCEEDED(status)) status = link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(file.put()));
    if (SUCCEEDED(status)) status = file->Save(shortcut.c_str(), TRUE);
    if (SUCCEEDED(status)) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return status;
}
} // namespace

class WindowsNotifications::Impl {
public:
    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(failures->mutex);
            failures->target = nullptr;
            failures->message = 0;
        }
        subscriptions.clear();
    }
    bool initialize() {
        HRESULT status = runtime.initialize();
        if (FAILED(status)) return fail(L"Could not initialize Windows notifications", status);
        if (!registered) {
            status = registerIdentity();
            if (FAILED(status)) return fail(L"Could not register this executable for notifications", status);
            registered = true;
        }
        return true;
    }
    bool fail(const wchar_t* operation, HRESULT status) {
        error = errorText(operation, status);
        return false;
    }
    Runtime runtime;
    bool registered = false;
    std::wstring error;
    std::shared_ptr<FailureState> failures = std::make_shared<FailureState>();
    std::map<std::wstring, std::unique_ptr<Subscription>> subscriptions;
};

WindowsNotifications::WindowsNotifications() : impl_(std::make_unique<Impl>()) {}
WindowsNotifications::~WindowsNotifications() = default;
const std::wstring& WindowsNotifications::lastError() const noexcept { return impl_->error; }
void WindowsNotifications::setFailureTarget(HWND window, UINT message) {
    std::lock_guard<std::mutex> lock(impl_->failures->mutex);
    impl_->failures->target = window;
    impl_->failures->message = message;
}
std::wstring WindowsNotifications::takeFailure() {
    std::lock_guard<std::mutex> lock(impl_->failures->mutex);
    std::wstring result;
    const auto ticket = impl_->failures->errorTicket.lock();
    if (ticket && ticket->active) result.swap(impl_->failures->error);
    impl_->failures->error.clear();
    impl_->failures->errorTicket.reset();
    return result;
}

bool WindowsNotifications::show(const std::wstring& xml, const std::wstring& tag) {
    impl_->error.clear();
    if (tag != L"pomodoro" && tag != L"countdown" && tag != L"test")
        return impl_->fail(L"Unsupported notification tag", E_INVALIDARG);
    if (!impl_->initialize()) return false;
    auto& runtime = impl_->runtime;
    ComPtr<Manager> manager;
    HRESULT status = factory(runtime, managerClass, managerId, manager);
    if (FAILED(status)) return impl_->fail(L"Could not open Windows notification service", status);
    String identity(runtime);
    status = identity.set(appId);
    ComPtr<Notifier> notifier;
    if (SUCCEEDED(status)) status = manager->CreateToastNotifierWithId(identity.get(), notifier.put());
    if (FAILED(status)) return impl_->fail(L"Could not create this application's notifier", status);
    const wchar_t* sendOperation = L"Could not create the Windows notification";
    const auto result = requestNotification(
        [&notifier](int& setting) { return static_cast<std::int32_t>(notifier->get_Setting(&setting)); },
        [&]() -> std::int32_t {
            ComPtr<IInspectable> toast;
            HRESULT sendStatus = createToast(runtime, xml, tag, toast);
            if (FAILED(sendStatus)) return static_cast<std::int32_t>(sendStatus);
            sendOperation = L"Could not monitor Windows notification delivery";
            auto subscription = std::make_unique<Subscription>(impl_->failures);
            sendStatus = subscription->connect(toast.get());
            if (FAILED(sendStatus)) return static_cast<std::int32_t>(sendStatus);
            // Retire the old delegate before sending its replacement; delayed
            // callbacks from cleared/replaced notices cannot replace its error.
            impl_->subscriptions.erase(tag);
            impl_->subscriptions.emplace(tag, std::move(subscription));
            sendOperation = L"Windows could not accept the notification";
            sendStatus = notifier->Show(toast.get());
            if (FAILED(sendStatus)) impl_->subscriptions.erase(tag);
            return static_cast<std::int32_t>(sendStatus);
        });
    if (result.state == NotificationRequestState::SettingsReadFailed)
        return impl_->fail(L"Could not read Windows notification settings", static_cast<HRESULT>(result.status));
    if (result.state == NotificationRequestState::Blocked) {
        switch (result.setting) {
        case 1: impl_->error = L"Notifications for Pomodoro are disabled in Windows Settings. Enable Pomodoro under System > Notifications."; break;
        case 2: impl_->error = L"Windows notifications are disabled for this user. Enable notifications in Windows Settings > System > Notifications."; break;
        case 3: impl_->error = L"A Windows policy has disabled notifications. Contact your administrator."; break;
        case 4: impl_->error = L"This Windows notification manifest setting does not permit notifications."; break;
        default: impl_->error = L"Windows reports an unsupported notification setting: " + std::to_wstring(result.setting); break;
        }
        return false;
    }
    if (result.state == NotificationRequestState::SendFailed)
        return impl_->fail(sendOperation, static_cast<HRESULT>(result.status));
    return true;
}

bool WindowsNotifications::clear(const std::wstring& tag) {
    impl_->error.clear();
    impl_->subscriptions.erase(tag);
    if (!impl_->initialize()) return false;
    auto& runtime = impl_->runtime;
    ComPtr<Manager2> manager;
    HRESULT status = factory(runtime, managerClass, manager2Id, manager);
    ComPtr<History> history;
    if (SUCCEEDED(status)) status = manager->get_History(history.put());
    String tagString(runtime), groupString(runtime), identity(runtime);
    if (SUCCEEDED(status)) status = tagString.set(tag);
    if (SUCCEEDED(status)) status = groupString.set(group);
    if (SUCCEEDED(status)) status = identity.set(appId);
    if (SUCCEEDED(status)) status = history->RemoveGroupedTagWithId(tagString.get(), groupString.get(), identity.get());
    if (FAILED(status)) return impl_->fail(L"Could not remove the notification from Windows notification history", status);
    return true;
}

bool validateNotificationXml(const std::wstring& xml, std::wstring& error) {
    Runtime runtime;
    HRESULT status = runtime.initialize();
    ComPtr<IInspectable> toast;
    Subscription subscription(std::make_shared<FailureState>());
    if (SUCCEEDED(status)) status = createToast(runtime, xml, L"validation", toast);
    if (SUCCEEDED(status)) status = subscription.connect(toast.get());
    error = FAILED(status) ? errorText(L"Windows notification content validation failed", status) : L"";
    return SUCCEEDED(status);
}

} // namespace pomodoro
#endif
