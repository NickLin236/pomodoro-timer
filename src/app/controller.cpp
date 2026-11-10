#include "pomodoro/controller.h"
#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>

namespace pomodoro {

Controller::Controller(StateStore& store, Clock& clock) : store_(store), clock_(clock) {}

Update Controller::load() {
    auto settings = store_.loadPreferences();
    preferences_ = settings.value;
    auto state = store_.loadState();
    engine_.pomodoro = state.value.pomodoro;
    engine_.countdown = state.value.countdown;
    pomodoroAttempt_ = state.value.pomodoroNotificationSequence;
    countdownAttempt_ = state.value.countdownNotificationSequence;
    if (engine_.pomodoro.status == Status::Idle) engine_.resetPomodoro(preferences_.pomodoro);
    if (engine_.countdown.status == Status::Idle) engine_.resetCountdown(preferences_.countdownDurationMs);
    warning_ = settings.warning;
    if (!state.warning.empty()) {
        if (!warning_.empty()) warning_ += "\n";
        warning_ += state.warning;
    }
    return poll(true);
}

void Controller::sampleTime() {
    const auto sample = clock_.now();
    if (!sampled_) {
        anchor_ = sample;
        now_ = sample.utcMs;
        lastPollMonotonic_ = sample.monotonicMs;
        sampled_ = true;
        return;
    }
    // Real Windows FILETIME/interrupt-time values are well inside int64 range.
    const Millis elapsed = sample.monotonicMs - anchor_.monotonicMs;
    const Millis projected = anchor_.utcMs + elapsed;
    const Millis shift = sample.utcMs - projected;
    if (shift >= 1000 || shift <= -1000 || elapsed < 0) {
        if (!engine_.shiftDeadlines(shift)) {
            warning_ = "Clock change exceeds the supported date range. Timer state was not shifted.";
        }
        anchor_ = sample;
        now_ = sample.utcMs;
        clockShifted_ = true;
    } else {
        now_ = projected;
    }
}

Update Controller::poll(bool recovering) {
    sampleTime();
    const Millis currentMonotonic = anchor_.monotonicMs + (now_ - anchor_.utcMs);
    const bool late = currentMonotonic - lastPollMonotonic_ > 2000;
    lastPollMonotonic_ = currentMonotonic;
    auto events = engine_.advance(now_);
    Update update;
    update.recovery = recovering || late || events.size() > 2;
    for (const auto& event : events) {
        auto& attempted = event.timer == TimerKind::Pomodoro ? pomodoroAttempt_ : countdownAttempt_;
        if (event.sequence > attempted) {
            update.events.push_back(event);
            attempted = event.sequence;
        }
    }
    // Record notification attempts before handing events to the shell. A crash
    // in between can suppress one banner, but will not routinely repeat it.
    if (!events.empty() || clockShifted_) save();
    clockShifted_ = false;
    return update;
}

std::string Controller::newSessionId() {
    // Time plus per-process counter is sufficient to distinguish this single
    // instance's sessions; event sequences are scoped by the saved session ID.
    return std::to_string(now_) + "-" + std::to_string(++sessionCounter_);
}

bool Controller::startPomodoro(const PomodoroConfig& config) {
    if (!engine_.startPomodoro(config, now_, newSessionId())) return false;
    pomodoroAttempt_ = 0;
    save();
    return true;
}

bool Controller::startCountdown(Millis duration) {
    if (!engine_.startCountdown(duration, now_, newSessionId())) return false;
    countdownAttempt_ = 0;
    save();
    return true;
}

void Controller::togglePomodoro() {
    if (engine_.pomodoro.status == Status::Running) engine_.pausePomodoro(now_);
    else if (engine_.pomodoro.status == Status::Paused) engine_.resumePomodoro(now_);
    save();
}
void Controller::toggleCountdown() {
    if (engine_.countdown.status == Status::Running) engine_.pauseCountdown(now_);
    else if (engine_.countdown.status == Status::Paused) engine_.resumeCountdown(now_);
    save();
}
void Controller::skipPomodoro() { engine_.skipPomodoro(now_); save(); }
void Controller::resetPomodoro() {
    engine_.resetPomodoro(preferences_.pomodoro); pomodoroAttempt_ = 0; save();
}
void Controller::resetCountdown() {
    engine_.resetCountdown(preferences_.countdownDurationMs); countdownAttempt_ = 0; save();
}

bool Controller::savePreferences(const Preferences& preferences) {
    if (!validateConfig(preferences.pomodoro) || !validateDuration(preferences.countdownDurationMs)) return false;
    try {
        store_.savePreferences(preferences);
        preferences_ = preferences;
        if (engine_.pomodoro.status == Status::Idle) engine_.resetPomodoro(preferences_.pomodoro);
        if (engine_.countdown.status == Status::Idle) engine_.resetCountdown(preferences_.countdownDurationMs);
        return save();
    } catch (const std::exception& error) {
        warning_ = error.what();
        return false;
    }
}

bool Controller::save() {
    AppState state;
    state.pomodoro = engine_.pomodoro;
    state.countdown = engine_.countdown;
    state.savedAtUtcMs = now_;
    state.pomodoroNotificationSequence = pomodoroAttempt_;
    state.countdownNotificationSequence = countdownAttempt_;
    try { store_.saveState(state); return true; }
    catch (const std::exception& error) { warning_ = error.what(); return false; }
}

bool Controller::running() const noexcept {
    return engine_.pomodoro.status == Status::Running || engine_.countdown.status == Status::Running;
}

} // namespace pomodoro
