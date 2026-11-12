#include "pomodoro/windows_shell.h"

#ifdef _WIN32
#include <algorithm>
#include <iterator>
#include <vector>

namespace pomodoro {
namespace {

// Fixed identity preserves the user's notification-area placement preference.
constexpr GUID trayGuid = {0x986ebc4a, 0x5392, 0x4d3b,
                           {0xb1, 0x98, 0xda, 0x8f, 0x18, 0x30, 0xe8, 0x51}};
constexpr wchar_t startupKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t startupValue[] = L"PomodoroNative";
constexpr int applicationIconResource = 101;

template <std::size_t N>
void copyText(wchar_t (&destination)[N], const std::wstring& text) {
    const auto length = (std::min)(N - 1, text.size());
    std::copy_n(text.c_str(), length, destination);
    destination[length] = L'\0';
}

class RegistryKey final {
public:
    ~RegistryKey() { if (value != nullptr) RegCloseKey(value); }
    HKEY value = nullptr;
};

std::wstring systemError(LSTATUS code) {
    wchar_t* buffer = nullptr;
    const DWORD count = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                           FORMAT_MESSAGE_FROM_SYSTEM |
                                           FORMAT_MESSAGE_IGNORE_INSERTS,
                                       nullptr, static_cast<DWORD>(code), 0,
                                       reinterpret_cast<wchar_t*>(&buffer), 0,
                                       nullptr);
    std::wstring message = count != 0 ? std::wstring(buffer, count)
                                     : L"Windows error " + std::to_wstring(code);
    if (buffer != nullptr) LocalFree(buffer);
    return message;
}

bool executablePath(std::wstring& path) {
    std::vector<wchar_t> buffer(512);
    while (buffer.size() <= 32768) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        if (length == 0) return false;
        if (length < buffer.size()) {
            path.assign(buffer.data(), length);
            return true;
        }
        buffer.resize(buffer.size() * 2);
    }
    SetLastError(ERROR_FILENAME_EXCED_RANGE);
    return false;
}

bool startupCommand(std::wstring& command) {
    std::wstring path;
    if (!executablePath(path)) return false;
    command = L"\"" + path + L"\" --background";
    return true;
}

} // namespace

ShellIntegration::ShellIntegration(HINSTANCE instance, HWND window,
                                   UINT callbackMessage)
    : window_(window), callbackMessage_(callbackMessage) {
    icon_ = static_cast<HICON>(LoadImageW(instance,
        MAKEINTRESOURCEW(applicationIconResource), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    ownsIcon_ = icon_ != nullptr;
    if (icon_ == nullptr) icon_ = LoadIconW(nullptr, IDI_APPLICATION);
}

ShellIntegration::~ShellIntegration() {
    if (trayInstalled_) {
        auto data = iconData();
        Shell_NotifyIconW(NIM_DELETE, &data);
    }
    if (taskbar_ != nullptr) {
        taskbar_->SetProgressState(window_, TBPF_NOPROGRESS);
        taskbar_->Release();
    }
    if (ownsIcon_ && icon_ != nullptr) DestroyIcon(icon_);
}

NOTIFYICONDATAW ShellIntegration::iconData() const noexcept {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = 1;
    data.uFlags = NIF_GUID;
    data.guidItem = trayGuid;
    return data;
}

bool ShellIntegration::installTray() {
    if (trayInstalled_) return true;
    auto data = iconData();
    data.uFlags |= NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = callbackMessage_;
    data.hIcon = icon_;
    copyText(data.szTip, tooltip_);
    if (!Shell_NotifyIconW(NIM_ADD, &data)) return false;
    trayInstalled_ = true;
    data = iconData();
    data.uVersion = NOTIFYICON_VERSION_4;
    if (!Shell_NotifyIconW(NIM_SETVERSION, &data)) {
        // The target is Windows 10, so falling back to legacy callback semantics
        // would leave an unusable tray icon. Make failure visible to the caller.
        Shell_NotifyIconW(NIM_DELETE, &data);
        trayInstalled_ = false;
        return false;
    }
    return true;
}

bool ShellIntegration::restoreTray() {
    // TaskbarCreated means Explorer lost all old notification-area entries.
    trayInstalled_ = false;
    return installTray();
}

void ShellIntegration::updateTooltip(const std::wstring& tooltip) {
    tooltip_ = tooltip;
    if (!trayInstalled_) return;
    auto data = iconData();
    data.uFlags |= NIF_TIP | NIF_SHOWTIP;
    copyText(data.szTip, tooltip_);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void ShellIntegration::initializeTaskbar() {
    if (taskbar_ != nullptr) {
        taskbar_->Release();
        taskbar_ = nullptr;
    }
    if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(&taskbar_)))) return;
    if (FAILED(taskbar_->HrInit())) {
        taskbar_->Release();
        taskbar_ = nullptr;
    }
}

void ShellIntegration::setTaskbar(ULONGLONG completed, ULONGLONG total,
                                 bool paused, bool active) {
    if (taskbar_ == nullptr) return;
    if (!active || total == 0) {
        clearTaskbar();
        return;
    }
    taskbar_->SetProgressState(window_, paused ? TBPF_PAUSED : TBPF_NORMAL);
    taskbar_->SetProgressValue(window_, (std::min)(completed, total), total);
}

void ShellIntegration::clearTaskbar() {
    if (taskbar_ != nullptr) taskbar_->SetProgressState(window_, TBPF_NOPROGRESS);
}

bool setStartAtSignIn(bool enabled, std::wstring& error) {
    error.clear();
    RegistryKey key;
    LSTATUS status;
    if (enabled) {
        status = RegCreateKeyExW(HKEY_CURRENT_USER, startupKey, 0, nullptr, 0,
                                 KEY_SET_VALUE, nullptr, &key.value, nullptr);
    } else {
        status = RegOpenKeyExW(HKEY_CURRENT_USER, startupKey, 0, KEY_SET_VALUE,
                               &key.value);
        if (status == ERROR_FILE_NOT_FOUND) return true;
    }
    if (status != ERROR_SUCCESS) {
        error = L"Could not open sign-in startup settings: " + systemError(status);
        return false;
    }
    if (enabled) {
        std::wstring command;
        if (!startupCommand(command)) {
            error = L"Could not find this executable: " + systemError(GetLastError());
            return false;
        }
        status = RegSetValueExW(key.value, startupValue, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(command.c_str()),
                                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key.value, startupValue);
        if (status == ERROR_FILE_NOT_FOUND) return true;
    }
    if (status != ERROR_SUCCESS) {
        error = L"Could not update sign-in startup settings: " + systemError(status);
        return false;
    }
    return true;
}

bool isStartAtSignInEnabled() {
    RegistryKey key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, startupKey, 0, KEY_QUERY_VALUE,
                      &key.value) != ERROR_SUCCESS) return false;
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key.value, startupValue, nullptr, &type, nullptr, &bytes)
        != ERROR_SUCCESS || type != REG_SZ || bytes < sizeof(wchar_t) ||
        bytes > 65536 || bytes % sizeof(wchar_t) != 0) return false;
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key.value, startupValue, nullptr, &type,
                         reinterpret_cast<BYTE*>(buffer.data()), &bytes)
        != ERROR_SUCCESS || type != REG_SZ) return false;
    std::wstring expected;
    return startupCommand(expected) && std::wstring(buffer.data()) == expected;
}

} // namespace pomodoro
#endif // _WIN32
