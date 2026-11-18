#include "pomodoro/notification_sound.h"

#ifdef _WIN32
#include "pomodoro/audio_gain.h"
#include <windows.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <utility>
#include <vector>

namespace pomodoro {
namespace {
// SND_SYSTEM is supported since Vista but absent from some older MinGW headers.
constexpr DWORD systemSoundSession = 0x00200000;
[[noreturn]] void failed(const char* operation, DWORD error = GetLastError()) {
    throw std::runtime_error(std::string(operation) + " (Windows error " + std::to_string(error) + ").");
}
class Handle {
public:
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    HANDLE get() const { return value_; }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
private:
    HANDLE value_;
};

std::wstring defaultSoundPath() {
    std::vector<wchar_t> directory(32768);
    const auto count = GetWindowsDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
    if (!count || count >= directory.size()) failed("Could not locate the Windows sound folder");
    return (std::filesystem::path(std::wstring(directory.data(), count)) /
        L"Media" / L"Windows Notify System Generic.wav").wstring();
}

std::wstring configuredSoundPath() {
    HKEY key = nullptr;
    const auto opened = RegOpenKeyExW(HKEY_CURRENT_USER,
        L"AppEvents\\Schemes\\Apps\\.Default\\Notification.Default\\.Current",
        0, KEY_QUERY_VALUE, &key);
    if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND) return defaultSoundPath();
    if (opened != ERROR_SUCCESS) failed("Could not read the configured notification sound", opened);
    struct CloseKey { HKEY key; ~CloseKey() { RegCloseKey(key); } } close{key};
    DWORD type = 0, size = 0;
    auto status = RegQueryValueExW(key, nullptr, nullptr, &type, nullptr, &size);
    if (status == ERROR_FILE_NOT_FOUND) return defaultSoundPath();
    if (status != ERROR_SUCCESS) failed("Could not read the configured notification sound", status);
    if ((type != REG_SZ && type != REG_EXPAND_SZ) || size > 65536 || size % sizeof(wchar_t))
        throw std::runtime_error("The configured notification sound path is invalid.");
    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1, L'\0');
    status = RegQueryValueExW(key, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()), &size);
    if (status != ERROR_SUCCESS) failed("Could not read the configured notification sound", status);
    if ((type != REG_SZ && type != REG_EXPAND_SZ) || size % sizeof(wchar_t) ||
        size / sizeof(wchar_t) >= buffer.size())
        throw std::runtime_error("The configured notification sound path is invalid.");
    std::wstring path(buffer.data(), size / sizeof(wchar_t));
    while (!path.empty() && path.back() == L'\0') path.pop_back();
    if (path.find(L'\0') != std::wstring::npos)
        throw std::runtime_error("The configured notification sound path is invalid.");
    // An explicit empty value is the user's None selection, not a missing file.
    if (path.empty()) return {};
    const auto required = ExpandEnvironmentStringsW(path.c_str(), nullptr, 0);
    if (!required || required > 32768) failed("Could not expand the notification sound path");
    std::vector<wchar_t> expanded(required);
    const auto count = ExpandEnvironmentStringsW(path.c_str(), expanded.data(), required);
    if (!count || count > required) failed("Could not expand the notification sound path");
    return std::wstring(expanded.data());
}

std::vector<std::uint8_t> readSound(const std::wstring& path) {
    // Exclude relative, UNC and device paths, including remote mapped drives.
    // This feature must not fetch media over a network.
    if (path.size() < 3 || !((path[0] >= L'A' && path[0] <= L'Z') ||
        (path[0] >= L'a' && path[0] <= L'z')) || path[1] != L':' ||
        (path[2] != L'\\' && path[2] != L'/'))
        throw std::runtime_error("Notification sound must be a file on a local drive.");
    const wchar_t root[]{path[0], L':', L'\\', L'\0'};
    const auto drive = GetDriveTypeW(root);
    if (drive != DRIVE_FIXED && drive != DRIVE_REMOVABLE && drive != DRIVE_RAMDISK && drive != DRIVE_CDROM)
        throw std::runtime_error("Notification sound must be a file on a local drive.");
    const std::filesystem::path local(path);
    auto prefix = local.root_path();
    // Check each component before traversing the next one. Opening only the
    // final component with OPEN_REPARSE_POINT would still follow an earlier
    // directory junction, which could lead to a network share. These checks do
    // not claim race-proof isolation from another process controlling this user.
    for (const auto& component : local.relative_path()) {
        if (component == L"." || component == L"..")
            throw std::runtime_error("Notification sound path must not contain relative components.");
        prefix /= component;
        const auto attributes = GetFileAttributesW(prefix.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) failed("Could not inspect the notification sound path");
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            throw std::runtime_error("Notification sound path must not contain links or reparse points.");
    }
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) failed("Could not open the configured notification sound");
    if (GetFileType(file.get()) != FILE_TYPE_DISK)
        throw std::runtime_error("The configured notification sound is not a regular audio file.");
    const auto required = GetFinalPathNameByHandleW(file.get(), nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!required || required > 32768) failed("Could not verify the notification sound file location");
    std::vector<wchar_t> finalPath(required + 1);
    const auto count = GetFinalPathNameByHandleW(file.get(), finalPath.data(), static_cast<DWORD>(finalPath.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!count || count >= finalPath.size()) failed("Could not verify the notification sound file location");
    const std::wstring final(finalPath.data(), count);
    if (final.size() < 7 || final.compare(0, 4, L"\\\\?\\") != 0 || final[5] != L':' || final[6] != L'\\')
        throw std::runtime_error("Notification sound resolved outside a local drive.");
    const wchar_t finalRoot[]{final[4], L':', L'\\', L'\0'};
    const auto finalDrive = GetDriveTypeW(finalRoot);
    if (finalDrive != DRIVE_FIXED && finalDrive != DRIVE_REMOVABLE && finalDrive != DRIVE_RAMDISK &&
        finalDrive != DRIVE_CDROM)
        throw std::runtime_error("Notification sound resolved outside a local drive.");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size)) failed("Could not read notification sound size");
    if (size.QuadPart <= 0 || size.QuadPart > static_cast<LONGLONG>(maxNotificationSoundBytes))
        throw std::runtime_error("Notification sound is empty or larger than 8 MiB.");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.QuadPart));
    std::size_t at = 0;
    while (at < bytes.size()) {
        DWORD count = 0;
        if (!ReadFile(file.get(), bytes.data() + at, static_cast<DWORD>(bytes.size() - at), &count, nullptr))
            failed("Could not read the configured notification sound");
        if (!count) throw std::runtime_error("The notification sound file changed while being read.");
        at += count;
    }
    return bytes;
}
} // namespace

class NotificationSound::Impl {
public:
    using Play = BOOL (WINAPI*)(LPCWSTR, HMODULE, DWORD);
    ~Impl() { if (module) FreeLibrary(module); }
    void initialize() {
        if (play) return;
        module = LoadLibraryExW(L"winmm.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) failed("Could not load the Windows sound service");
        const auto address = GetProcAddress(module, "PlaySoundW");
        static_assert(sizeof(address) == sizeof(play));
        std::memcpy(&play, &address, sizeof(play));
        if (!play) {
            const auto error = GetLastError();
            FreeLibrary(module); module = nullptr;
            failed("Could not open the Windows sound service", error);
        }
    }
    HMODULE module = nullptr;
    Play play = nullptr;
    std::vector<std::uint8_t> playing;
    std::wstring error;
};

NotificationSound::NotificationSound() : impl_(std::make_unique<Impl>()) {}
NotificationSound::~NotificationSound() { stop(); }

void NotificationSound::stop() noexcept {
    if (impl_->play && !impl_->playing.empty()) impl_->play(nullptr, nullptr, 0);
    impl_->playing.clear();
}

bool NotificationSound::play(int volumePercent) {
    impl_->error.clear();
    try {
        if (volumePercent < 0 || volumePercent > 200)
            throw std::runtime_error("Notification volume must be between 0% and 200%.");
        stop();
        // Native Windows toast audio remains responsible for exactly 100%.
        if (volumePercent == 0 || volumePercent == 100) return true;
        QUERY_USER_NOTIFICATION_STATE state{};
        const HRESULT queried = SHQueryUserNotificationState(&state);
        if (FAILED(queried)) throw std::runtime_error("Could not check whether alert sound is appropriate.");
        if (state != QUNS_ACCEPTS_NOTIFICATIONS) return true;
        const auto path = configuredSoundPath();
        if (path.empty()) return true;
        auto wave = applyWaveGain(readSound(path), volumePercent);
        impl_->initialize();
        impl_->playing = std::move(wave);
        if (!impl_->play(reinterpret_cast<LPCWSTR>(impl_->playing.data()), nullptr,
            SND_MEMORY | SND_ASYNC | SND_NODEFAULT | systemSoundSession)) {
            stop();
            throw std::runtime_error("Windows could not play the adjusted notification sound.");
        }
        return true;
    } catch (const std::exception& error) {
        const std::string message = error.what();
        impl_->error.assign(message.begin(), message.end());
        return false;
    }
}

const std::wstring& NotificationSound::lastError() const noexcept { return impl_->error; }

} // namespace pomodoro
#endif
