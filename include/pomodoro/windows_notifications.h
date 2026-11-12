#pragma once

#ifdef _WIN32
#include <windows.h>
#include <memory>
#include <string>

namespace pomodoro {

// Uses the Windows toast service and this executable's per-user protocol handler.
// Construction has no registration effects; show/clear prepare the identity.
class WindowsNotifications final {
public:
    WindowsNotifications();
    ~WindowsNotifications();
    WindowsNotifications(const WindowsNotifications&) = delete;
    WindowsNotifications& operator=(const WindowsNotifications&) = delete;
    bool show(const std::wstring& xml, const std::wstring& tag);
    bool clear(const std::wstring& tag);
    // The callback posts this message without pointers; takeFailure consumes it.
    void setFailureTarget(HWND window, UINT message);
    std::wstring takeFailure();
    const std::wstring& lastError() const noexcept;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Exercises Windows XML/toast creation without registration or showing a toast.
bool validateNotificationXml(const std::wstring& xml, std::wstring& error);

} // namespace pomodoro
#endif
