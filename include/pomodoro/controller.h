#pragma once

#include "pomodoro/clock.h"
#include "pomodoro/state_store.h"
#include <string>
#include <vector>

namespace pomodoro {

struct Update {
    std::vector<CompletionEvent> events;
    bool recovery = false;
};

// Owns no Windows resources. The UI polls before commands; commands use the last
// sampled time, so one clock sample drives each message-loop transaction.
class Controller {
public:
    Controller(StateStore& store, Clock& clock);
    Update load();
    Update poll(bool recovering = false);
    bool startPomodoro(const PomodoroConfig& config);
    bool startCountdown(Millis duration);
    void togglePomodoro();
    void toggleCountdown();
    void skipPomodoro();
    void resetPomodoro();
    void resetCountdown();
    bool savePreferences(const Preferences& preferences);
    bool save();
    bool running() const noexcept;
    Millis now() const noexcept { return now_; }
    const TimerEngine& engine() const noexcept { return engine_; }
    const Preferences& preferences() const noexcept { return preferences_; }
    const std::string& warning() const noexcept { return warning_; }
    void clearWarning() { warning_.clear(); }

private:
    void sampleTime();
    std::string newSessionId();
    StateStore& store_;
    Clock& clock_;
    TimerEngine engine_;
    Preferences preferences_;
    ClockSample anchor_{};
    Millis now_ = 0;
    Millis lastPollMonotonic_ = 0;
    std::uint64_t pomodoroAttempt_ = 0;
    std::uint64_t countdownAttempt_ = 0;
    std::uint64_t sessionCounter_ = 0;
    std::string warning_;
    bool sampled_ = false;
    bool clockShifted_ = false;
};

} // namespace pomodoro
