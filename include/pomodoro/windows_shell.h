#pragma once

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <string>

namespace pomodoro {

// Owns the shell icon and COM taskbar interface; COM must be initialized by the
// caller on this thread. Window/menu ownership remains with MainWindow.
class ShellIntegration final {
public:
    ShellIntegration(HINSTANCE instance, HWND window, UINT callbackMessage);
    ~ShellIntegration();
    ShellIntegration(const ShellIntegration&) = delete;
    ShellIntegration& operator=(const ShellIntegration&) = delete;

    [[nodiscard]] bool installTray();
    [[nodiscard]] bool restoreTray();
    [[nodiscard]] bool trayInstalled() const noexcept { return trayInstalled_; }
    void updateTooltip(const std::wstring& tooltip);
    [[nodiscard]] bool notify(const std::wstring& title, const std::wstring& body,
                              bool sound);
    void initializeTaskbar();
    void setTaskbar(ULONGLONG completed, ULONGLONG total, bool paused, bool active);
    void clearTaskbar();

private:
    [[nodiscard]] NOTIFYICONDATAW iconData() const noexcept;
    HWND window_ = nullptr;
    UINT callbackMessage_ = 0;
    HICON icon_ = nullptr;
    bool ownsIcon_ = false;
    bool trayInstalled_ = false;
    ITaskbarList3* taskbar_ = nullptr;
    std::wstring tooltip_ = L"Pomodoro";
};

// Registers only this application's HKCU Run value. Failure leaves the caller
// responsible for showing error and keeping its preference checkbox accurate.
[[nodiscard]] bool setStartAtSignIn(bool enabled, std::wstring& error);
[[nodiscard]] bool isStartAtSignInEnabled();

} // namespace pomodoro
#endif // _WIN32
