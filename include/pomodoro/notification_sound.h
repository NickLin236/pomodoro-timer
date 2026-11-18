#pragma once

#ifdef _WIN32
#include <memory>
#include <string>

namespace pomodoro {

// Adjusted-volume playback only. The caller retains native toast audio at 100%
// and sends silent toast audio for the other volume levels. This helper never
// changes Windows mixer settings. Accepted toasts do not prove banner display;
// custom playback cannot reproduce every Focus assist/per-app sound policy.
class NotificationSound final {
public:
    NotificationSound();
    ~NotificationSound();
    NotificationSound(const NotificationSound&) = delete;
    NotificationSound& operator=(const NotificationSound&) = delete;
    bool play(int volumePercent);
    void stop() noexcept;
    const std::wstring& lastError() const noexcept;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pomodoro
#endif
