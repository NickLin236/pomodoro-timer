#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pomodoro {

using Millis = std::int64_t;
inline constexpr Millis maxDurationMs = 7LL * 24 * 60 * 60 * 1000;
inline constexpr int maxCycles = 1000;

enum class Status { Idle, Running, Paused, Completed };
enum class Phase { Focus, Break };
enum class TimerKind { Pomodoro, Countdown };

struct PomodoroConfig {
    Millis focusMs = 25 * 60 * 1000;
    Millis breakMs = 5 * 60 * 1000;
    int cycles = 4;
};

struct PomodoroState {
    std::string sessionId;
    Status status = Status::Idle;
    PomodoroConfig config;
    Phase phase = Phase::Focus;
    int cycle = 1;
    Millis phaseDurationMs = 25 * 60 * 1000;
    Millis deadlineUtcMs = 0;
    Millis remainingMs = 25 * 60 * 1000;
    std::uint64_t transitionSequence = 0;
};

struct CountdownState {
    std::string sessionId;
    Status status = Status::Idle;
    Millis durationMs = 5 * 60 * 1000;
    Millis deadlineUtcMs = 0;
    Millis remainingMs = 5 * 60 * 1000;
    std::uint64_t transitionSequence = 0;
};

struct CompletionEvent {
    TimerKind timer;
    Phase phase;
    int cycle;
    std::uint64_t sequence;
    bool final;
};

[[nodiscard]] bool validateDuration(Millis durationMs) noexcept;
[[nodiscard]] bool validateConfig(const PomodoroConfig& config) noexcept;

// Time is supplied by the caller: this engine reads no clock and performs no I/O.
// Call advance before processing UI commands so automatic completions are retained.
class TimerEngine {
public:
    PomodoroState pomodoro;
    CountdownState countdown;

    [[nodiscard]] std::vector<CompletionEvent> advance(Millis nowUtcMs);
    [[nodiscard]] bool startPomodoro(const PomodoroConfig& config, Millis nowUtcMs,
                                     std::string sessionId);
    [[nodiscard]] bool startCountdown(Millis durationMs, Millis nowUtcMs,
                                     std::string sessionId);
    void pausePomodoro(Millis nowUtcMs);
    void resumePomodoro(Millis nowUtcMs);
    void skipPomodoro(Millis nowUtcMs);
    void resetPomodoro(const PomodoroConfig& config);
    void pauseCountdown(Millis nowUtcMs);
    void resumeCountdown(Millis nowUtcMs);
    void resetCountdown(Millis durationMs);
    // UTC recovery may exceed the configured duration after a backwards clock
    // change while exited. Unrepresentable positive differences saturate at int64 max.
    [[nodiscard]] Millis pomodoroRemaining(Millis nowUtcMs) const noexcept;
    [[nodiscard]] Millis countdownRemaining(Millis nowUtcMs) const noexcept;

    // Returns false without mutation if any deadline or future phase would overflow.
    // The return may be ignored by callers whose clock range is already checked.
    bool shiftDeadlines(Millis deltaMs) noexcept;
};

} // namespace pomodoro
