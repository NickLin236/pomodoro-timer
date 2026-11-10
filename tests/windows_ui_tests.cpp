// Compile the actual command handler into this Windows-only integration test.
// No message loop, tray registration, sign-in launch, or real user Run key is
// used: the window stays hidden and HKCU is redirected for this process.
#include "../src/ui/main_window.cpp"

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

void startupSettingsReconcileExternalChanges() {
    RegistrySandbox registry;
    TemporaryData files;
    StateStore store(files.path);
    FakeClock clock({1'700'000'000'000LL, 1'000});
    MainWindow app(GetModuleHandleW(nullptr), store, clock);
    HiddenWindow hidden(app);
    const auto window = app.handle();
    const auto checkbox = GetDlgItem(window, StartupCheck);
    require(checkbox != nullptr, "Startup checkbox is missing");
    ComboBox_SetCurSel(GetDlgItem(window, BackgroundCombo), 0);
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

    Button_SetCheck(checkbox, BST_CHECKED);
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

    Button_SetCheck(checkbox, BST_UNCHECKED);
    saveSettings();
    require(!readStartup(), "Startup Off did not remove this application's value");
    require(!store.loadPreferences().value.startAtSignIn, "Startup Off preference did not persist");
    require(!IsWindowVisible(window), "Settings test unexpectedly showed the hidden window");
}
} // namespace

int main() {
    try {
        ComLifetime com;
        require(SUCCEEDED(com.status), "Could not initialize COM for native UI test");
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TAB_CLASSES | ICC_STANDARD_CLASSES};
        require(InitCommonControlsEx(&controls) != FALSE, "Could not initialize native controls");
        startupSettingsReconcileExternalChanges();
        std::cout << "Windows UI startup-registry regression tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Windows UI regression test failed: " << error.what() << '\n';
        return 1;
    }
}
