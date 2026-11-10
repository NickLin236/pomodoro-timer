#include "pomodoro/timer_engine.h"

#include <limits>
#include <utility>

namespace pomodoro {
namespace {

bool checkedAdd(Millis left, Millis right, Millis& result) noexcept {
    constexpr auto low = std::numeric_limits<Millis>::min();
    constexpr auto high = std::numeric_limits<Millis>::max();
    if ((right > 0 && left > high - right) ||
        (right < 0 && left < low - right)) {
        return false;
    }
    result = left + right;
    return true;
}

Millis remaining(Status status, Millis deadline, Millis pausedRemaining,
                 Millis now) noexcept {
    if (status == Status::Completed) return 0;
    if (status != Status::Running) return pausedRemaining;
    if (deadline <= now) return 0;
    // A wildly different wall clock must not overflow signed subtraction.
    if (now < 0 && deadline > std::numeric_limits<Millis>::max() + now) {
        return std::numeric_limits<Millis>::max();
    }
    // Restart recovery uses the persisted UTC deadline even if the clock was
    // moved backwards while exited. Clamping here would freeze the display at
    // the nominal duration and discard restored time when pausing.
    return deadline - now;
}

// Duration after the currently active phase, including the final break.
Millis futureDuration(const PomodoroState& state) noexcept {
    const auto laterCycles = static_cast<Millis>(state.config.cycles - state.cycle);
    return laterCycles * (state.config.focusMs + state.config.breakMs) +
           (state.phase == Phase::Focus ? state.config.breakMs : 0);
}

bool validId(const std::string& id) noexcept {
    return !id.empty() && id.size() <= 128;
}

void finishPomodoro(PomodoroState& state) noexcept {
    state.status = Status::Completed;
    state.remainingMs = 0;
    state.deadlineUtcMs = 0;
}

void nextPhase(PomodoroState& state) noexcept {
    if (state.phase == Phase::Focus) {
        state.phase = Phase::Break;
        state.phaseDurationMs = state.config.breakMs;
    } else {
        ++state.cycle;
        state.phase = Phase::Focus;
        state.phaseDurationMs = state.config.focusMs;
    }
    state.remainingMs = state.phaseDurationMs;
}

} // namespace

bool validateDuration(Millis durationMs) noexcept {
    return durationMs > 0 && durationMs <= maxDurationMs;
}

bool validateConfig(const PomodoroConfig& config) noexcept {
    return validateDuration(config.focusMs) && validateDuration(config.breakMs) &&
           config.cycles > 0 && config.cycles <= maxCycles;
}

std::vector<CompletionEvent> TimerEngine::advance(Millis nowUtcMs) {
    std::vector<CompletionEvent> events;
    while (pomodoro.status == Status::Running && pomodoro.deadlineUtcMs <= nowUtcMs) {
        const auto previousDeadline = pomodoro.deadlineUtcMs;
        const bool final = pomodoro.phase == Phase::Break &&
                           pomodoro.cycle == pomodoro.config.cycles;
        events.push_back({TimerKind::Pomodoro, pomodoro.phase, pomodoro.cycle,
                          ++pomodoro.transitionSequence, final});
        if (final) {
            finishPomodoro(pomodoro);
            break;
        }
        nextPhase(pomodoro);
        // start/resume/shift validate the entire schedule's arithmetic horizon.
        // Also guard restored data defensively instead of invoking signed overflow.
        if (!checkedAdd(previousDeadline, pomodoro.phaseDurationMs,
                        pomodoro.deadlineUtcMs)) {
            pomodoro.status = Status::Paused;
            pomodoro.deadlineUtcMs = 0;
            break;
        }
    }
    if (countdown.status == Status::Running && countdown.deadlineUtcMs <= nowUtcMs) {
        countdown.status = Status::Completed;
        countdown.remainingMs = 0;
        countdown.deadlineUtcMs = 0;
        events.push_back({TimerKind::Countdown, Phase::Focus, 1,
                          ++countdown.transitionSequence, true});
    }
    return events;
}

bool TimerEngine::startPomodoro(const PomodoroConfig& config, Millis nowUtcMs,
                                std::string sessionId) {
    if (!validateConfig(config) || !validId(sessionId) ||
        pomodoro.status == Status::Running || pomodoro.status == Status::Paused) {
        return false;
    }
    Millis scheduleEnd = 0;
    const Millis scheduleDuration = (config.focusMs + config.breakMs) * config.cycles;
    if (!checkedAdd(nowUtcMs, scheduleDuration, scheduleEnd)) return false;
    PomodoroState state;
    state.sessionId = std::move(sessionId);
    state.config = config;
    state.status = Status::Running;
    state.phaseDurationMs = config.focusMs;
    state.remainingMs = config.focusMs;
    if (!checkedAdd(nowUtcMs, config.focusMs, state.deadlineUtcMs)) return false;
    pomodoro = std::move(state);
    return true;
}

bool TimerEngine::startCountdown(Millis durationMs, Millis nowUtcMs,
                                std::string sessionId) {
    if (!validateDuration(durationMs) || !validId(sessionId) ||
        countdown.status == Status::Running || countdown.status == Status::Paused) {
        return false;
    }
    CountdownState state;
    state.sessionId = std::move(sessionId);
    state.status = Status::Running;
    state.durationMs = durationMs;
    state.remainingMs = durationMs;
    if (!checkedAdd(nowUtcMs, durationMs, state.deadlineUtcMs)) return false;
    countdown = std::move(state);
    return true;
}

void TimerEngine::pausePomodoro(Millis nowUtcMs) {
    if (pomodoro.status != Status::Running) return;
    const Millis left = pomodoroRemaining(nowUtcMs);
    // Due phases belong to advance(), which returns their completion events.
    if (left == 0) return;
    pomodoro.remainingMs = left;
    pomodoro.deadlineUtcMs = 0;
    pomodoro.status = Status::Paused;
}

void TimerEngine::resumePomodoro(Millis nowUtcMs) {
    if (pomodoro.status != Status::Paused) return;
    Millis deadline = 0;
    Millis scheduleEnd = 0;
    if (!checkedAdd(nowUtcMs, pomodoro.remainingMs, deadline) ||
        !checkedAdd(deadline, futureDuration(pomodoro), scheduleEnd)) return;
    pomodoro.deadlineUtcMs = deadline;
    pomodoro.status = Status::Running;
}

void TimerEngine::skipPomodoro(Millis nowUtcMs) {
    if (pomodoro.status != Status::Running && pomodoro.status != Status::Paused) return;
    if (pomodoro.phase == Phase::Break && pomodoro.cycle == pomodoro.config.cycles) {
        ++pomodoro.transitionSequence;
        finishPomodoro(pomodoro);
        return;
    }
    auto next = pomodoro;
    nextPhase(next);
    if (next.status == Status::Running) {
        Millis scheduleEnd = 0;
        if (!checkedAdd(nowUtcMs, next.phaseDurationMs, next.deadlineUtcMs) ||
            !checkedAdd(next.deadlineUtcMs, futureDuration(next), scheduleEnd)) return;
    }
    ++next.transitionSequence;
    pomodoro = std::move(next);
}

void TimerEngine::resetPomodoro(const PomodoroConfig& config) {
    if (!validateConfig(config)) return;
    pomodoro = {};
    pomodoro.config = config;
    pomodoro.phaseDurationMs = config.focusMs;
    pomodoro.remainingMs = config.focusMs;
}

void TimerEngine::pauseCountdown(Millis nowUtcMs) {
    if (countdown.status != Status::Running) return;
    const Millis left = countdownRemaining(nowUtcMs);
    if (left == 0) return;
    countdown.remainingMs = left;
    countdown.deadlineUtcMs = 0;
    countdown.status = Status::Paused;
}

void TimerEngine::resumeCountdown(Millis nowUtcMs) {
    if (countdown.status != Status::Paused) return;
    Millis deadline = 0;
    if (!checkedAdd(nowUtcMs, countdown.remainingMs, deadline)) return;
    countdown.deadlineUtcMs = deadline;
    countdown.status = Status::Running;
}

void TimerEngine::resetCountdown(Millis durationMs) {
    if (!validateDuration(durationMs)) return;
    countdown = {};
    countdown.durationMs = durationMs;
    countdown.remainingMs = durationMs;
}

Millis TimerEngine::pomodoroRemaining(Millis nowUtcMs) const noexcept {
    return remaining(pomodoro.status, pomodoro.deadlineUtcMs, pomodoro.remainingMs, nowUtcMs);
}

Millis TimerEngine::countdownRemaining(Millis nowUtcMs) const noexcept {
    return remaining(countdown.status, countdown.deadlineUtcMs, countdown.remainingMs, nowUtcMs);
}

bool TimerEngine::shiftDeadlines(Millis deltaMs) noexcept {
    Millis pomoDeadline = pomodoro.deadlineUtcMs;
    Millis countdownDeadline = countdown.deadlineUtcMs;
    Millis scheduleEnd = 0;
    if (pomodoro.status == Status::Running &&
        (!checkedAdd(pomoDeadline, deltaMs, pomoDeadline) ||
         !checkedAdd(pomoDeadline, futureDuration(pomodoro), scheduleEnd))) return false;
    if (countdown.status == Status::Running &&
        !checkedAdd(countdownDeadline, deltaMs, countdownDeadline)) return false;
    pomodoro.deadlineUtcMs = pomoDeadline;
    countdown.deadlineUtcMs = countdownDeadline;
    return true;
}

} // namespace pomodoro
