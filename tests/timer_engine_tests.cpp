#include "pomodoro/timer_engine.h"

#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>

using namespace pomodoro;

namespace {

struct FakeClock {
    Millis now = 1'700'000'000'000LL;
    void elapse(Millis duration) { now += duration; }
};

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string(__func__) + ":" + std::to_string(__LINE__) + ": " #condition); } while (false)

void finalBreakIsIncluded() {
    FakeClock clock;
    TimerEngine engine;
    CHECK(engine.startPomodoro({2'000, 1'000, 1}, clock.now, "one"));
    CHECK(engine.advance(clock.now).empty());
    clock.elapse(2'000);
    const auto focus = engine.advance(clock.now);
    CHECK(focus.size() == 1);
    CHECK(focus[0].phase == Phase::Focus && !focus[0].final && focus[0].sequence == 1);
    CHECK(engine.pomodoro.status == Status::Running);
    CHECK(engine.pomodoro.phase == Phase::Break);
    CHECK(engine.pomodoroRemaining(clock.now) == 1'000);
    clock.elapse(1'000);
    const auto final = engine.advance(clock.now);
    CHECK(final.size() == 1);
    CHECK(final[0].phase == Phase::Break && final[0].final && final[0].sequence == 2);
    CHECK(engine.pomodoro.status == Status::Completed);
    CHECK(engine.pomodoro.cycle == 1 && engine.pomodoro.phase == Phase::Break);
    CHECK(engine.pomodoroRemaining(clock.now) == 0);
    CHECK(engine.advance(clock.now).empty());
    clock.elapse(100'000);
    CHECK(engine.advance(clock.now).empty());
}

void customCyclesAndLateCallbacks() {
    FakeClock clock;
    const Millis start = clock.now;
    TimerEngine engine;
    CHECK(engine.startPomodoro({120'000, 60'000, 3}, clock.now, "custom"));
    clock.elapse(240'000);
    const auto events = engine.advance(clock.now);
    CHECK(events.size() == 2);
    CHECK(events[0].phase == Phase::Focus && events[0].cycle == 1);
    CHECK(events[1].phase == Phase::Break && events[1].cycle == 1);
    CHECK(engine.pomodoro.phase == Phase::Focus && engine.pomodoro.cycle == 2);
    CHECK(engine.pomodoroRemaining(clock.now) == 60'000);
    CHECK(engine.pomodoro.deadlineUtcMs == start + 300'000);
    CHECK(engine.advance(clock.now).empty());
    clock.now = start + 540'000;
    const auto rest = engine.advance(clock.now);
    CHECK(rest.size() == 4);
    CHECK(rest.back().final && rest.back().sequence == 6 && rest.back().cycle == 3);
    CHECK(engine.pomodoro.status == Status::Completed);
}

void pausePersistsAcrossAbsence() {
    FakeClock clock;
    TimerEngine engine;
    CHECK(engine.startPomodoro({600'000, 60'000, 2}, clock.now, "paused-pomo"));
    CHECK(engine.startCountdown(600'000, clock.now, "paused-countdown"));
    clock.elapse(100'123);
    engine.pausePomodoro(clock.now);
    engine.pauseCountdown(clock.now);
    CHECK(engine.pomodoroRemaining(clock.now) == 499'877);
    CHECK(engine.countdownRemaining(clock.now) == 499'877);
    TimerEngine restored = engine;
    clock.elapse(30LL * 24 * 60 * 60 * 1000);
    CHECK(restored.advance(clock.now).empty());
    CHECK(restored.pomodoroRemaining(clock.now) == 499'877);
    CHECK(restored.countdownRemaining(clock.now) == 499'877);
    restored.resumePomodoro(clock.now);
    restored.resumeCountdown(clock.now);
    CHECK(restored.pomodoro.status == Status::Running);
    CHECK(restored.countdown.status == Status::Running);
    clock.elapse(499'876);
    CHECK(restored.advance(clock.now).empty());
    CHECK(restored.pomodoroRemaining(clock.now) == 1);
    CHECK(restored.countdownRemaining(clock.now) == 1);
    clock.elapse(1);
    CHECK(restored.advance(clock.now).size() == 2);
    CHECK(restored.pomodoro.phase == Phase::Break);
    CHECK(restored.countdown.status == Status::Completed);
}

void trueExitKeepsDeadlines() {
    FakeClock clock;
    TimerEngine engine;
    CHECK(engine.startPomodoro({600'000, 300'000, 4}, clock.now, "exit-pomo"));
    CHECK(engine.startCountdown(600'000, clock.now, "exit-timer"));
    TimerEngine restored = engine;
    clock.elapse(300'000);
    CHECK(restored.advance(clock.now).empty());
    CHECK(restored.pomodoroRemaining(clock.now) == 300'000);
    CHECK(restored.countdownRemaining(clock.now) == 300'000);
    clock.elapse(600'001);
    CHECK(restored.advance(clock.now).size() == 3);
    CHECK(restored.pomodoro.phase == Phase::Focus && restored.pomodoro.cycle == 2);
    CHECK(restored.pomodoroRemaining(clock.now) == 599'999);
    CHECK(restored.countdown.status == Status::Completed);
}

void timersAreIndependent() {
    FakeClock clock;
    TimerEngine engine;
    CHECK(engine.startPomodoro({5'000, 2'000, 2}, clock.now, "independent-pomo"));
    CHECK(engine.startCountdown(1'000, clock.now, "independent-timer"));
    clock.elapse(1'000);
    const auto event = engine.advance(clock.now);
    CHECK(event.size() == 1 && event[0].timer == TimerKind::Countdown);
    CHECK(engine.pomodoroRemaining(clock.now) == 4'000);
    CHECK(engine.pomodoro.status == Status::Running);
    CHECK(engine.countdown.status == Status::Completed);
    CHECK(engine.startCountdown(10'000, clock.now, "second-countdown"));
    engine.pausePomodoro(clock.now);
    engine.resetCountdown(3'000);
    clock.elapse(5'000);
    CHECK(engine.advance(clock.now).empty());
    CHECK(engine.pomodoro.status == Status::Paused);
    CHECK(engine.countdown.status == Status::Idle);
    CHECK(engine.countdownRemaining(clock.now) == 3'000);
}

void resetSkipAndCapturedConfiguration() {
    FakeClock clock;
    TimerEngine engine;
    PomodoroConfig preferences{1'000, 500, 1};
    CHECK(engine.startPomodoro(preferences, clock.now, "skip"));
    preferences.focusMs = 9'000;
    CHECK(engine.pomodoro.config.focusMs == 1'000);
    clock.elapse(250);
    engine.skipPomodoro(clock.now);
    CHECK(engine.pomodoro.phase == Phase::Break);
    CHECK(engine.pomodoroRemaining(clock.now) == 500);
    CHECK(engine.pomodoro.transitionSequence == 1);
    CHECK(engine.advance(clock.now).empty());
    engine.pausePomodoro(clock.now);
    engine.skipPomodoro(clock.now);
    CHECK(engine.pomodoro.status == Status::Completed);
    CHECK(engine.pomodoro.transitionSequence == 2);
    CHECK(engine.advance(clock.now).empty());
    engine.resetPomodoro(preferences);
    CHECK(engine.pomodoro.status == Status::Idle);
    CHECK(engine.pomodoro.config.focusMs == 9'000);
    CHECK(engine.pomodoro.phase == Phase::Focus && engine.pomodoro.cycle == 1);
    CHECK(engine.pomodoro.sessionId.empty() && engine.pomodoro.transitionSequence == 0);
    CHECK(engine.pomodoroRemaining(clock.now) == 9'000);
    CHECK(engine.startPomodoro({100, 200, 2}, clock.now, "paused-skip"));
    engine.pausePomodoro(clock.now);
    engine.skipPomodoro(clock.now);
    CHECK(engine.pomodoro.status == Status::Paused);
    CHECK(engine.pomodoro.phase == Phase::Break);
    CHECK(engine.pomodoroRemaining(clock.now) == 200);
    CHECK(engine.pomodoro.deadlineUtcMs == 0);
}

void invalidInputPreservesState() {
    FakeClock clock;
    TimerEngine engine;
    CHECK(!validateDuration(0) && !validateDuration(-1));
    CHECK(!validateDuration(maxDurationMs + 1));
    CHECK(validateDuration(maxDurationMs));
    CHECK(!validateConfig({1, 1, 0}) && !validateConfig({1, 1, maxCycles + 1}));
    CHECK(!engine.startPomodoro({0, 100, 1}, clock.now, "bad"));
    CHECK(!engine.startCountdown(1, clock.now, ""));
    CHECK(engine.pomodoro.status == Status::Idle);
    CHECK(engine.startPomodoro({1'000, 200, 1}, clock.now, "original"));
    CHECK(engine.startCountdown(2'000, clock.now, "original-countdown"));
    const Millis originalDeadline = engine.pomodoro.deadlineUtcMs;
    CHECK(!engine.startPomodoro({10, 20, 2}, clock.now, "new"));
    CHECK(!engine.startCountdown(30, clock.now, "new-countdown"));
    engine.resetPomodoro({0, 20, 1});
    engine.resetCountdown(0);
    CHECK(engine.pomodoro.deadlineUtcMs == originalDeadline);
    CHECK(engine.pomodoro.sessionId == "original");
    CHECK(engine.countdown.durationMs == 2'000);
    engine.pausePomodoro(clock.now);
    CHECK(!engine.startPomodoro({10, 20, 2}, clock.now, "new"));
    CHECK(engine.pomodoro.status == Status::Paused);
}

void arithmeticBoundsAreChecked() {
    constexpr Millis high = std::numeric_limits<Millis>::max();
    constexpr Millis low = std::numeric_limits<Millis>::min();
    TimerEngine engine;
    CHECK(!engine.startCountdown(1'000, high - 999, "overflow"));
    CHECK(!engine.startPomodoro({1'000, 500, 2}, high - 2'999, "overflow"));
    CHECK(engine.startCountdown(1'000, high - 1'000, "exact-boundary"));
    CHECK(engine.countdown.deadlineUtcMs == high);
    CHECK(engine.countdownRemaining(low) == high); // Saturate unrepresentable UTC subtraction.
    engine.pauseCountdown(high - 500);
    engine.resumeCountdown(high - 499);
    CHECK(engine.countdown.status == Status::Paused);
    CHECK(engine.countdown.remainingMs == 500);
    engine.resumeCountdown(high - 500);
    CHECK(engine.countdown.status == Status::Running);
    CHECK(!engine.shiftDeadlines(1));
    CHECK(engine.countdown.deadlineUtcMs == high);
    CHECK(engine.advance(high).size() == 1);
    CHECK(engine.startPomodoro({1, 1, 1}, low, "near-min"));
    CHECK(engine.pomodoroRemaining(low) == 1);
    CHECK(engine.advance(low + 2).size() == 2);
}

void wallClockAdjustmentsPreserveRemaining() {
    FakeClock clock;
    TimerEngine engine;
    CHECK(engine.startPomodoro({60'000, 30'000, 2}, clock.now, "wall-pomo"));
    CHECK(engine.startCountdown(120'000, clock.now, "wall-countdown"));
    clock.elapse(20'000);
    CHECK(engine.pomodoroRemaining(clock.now) == 40'000);
    constexpr Millis adjustment = -3'600'000;
    CHECK(engine.shiftDeadlines(adjustment));
    clock.elapse(adjustment);
    CHECK(engine.pomodoroRemaining(clock.now) == 40'000);
    CHECK(engine.countdownRemaining(clock.now) == 100'000);
    engine.pausePomodoro(clock.now);
    const auto paused = engine.pomodoro;
    CHECK(engine.shiftDeadlines(3'600'000));
    CHECK(engine.pomodoro.deadlineUtcMs == paused.deadlineUtcMs);
    CHECK(engine.pomodoro.remainingMs == paused.remainingMs);
    const Millis timerDeadline = engine.countdown.deadlineUtcMs;
    CHECK(!engine.shiftDeadlines(std::numeric_limits<Millis>::max()));
    CHECK(engine.countdown.deadlineUtcMs == timerDeadline);
}

void maximumScheduleRecoveryIsBounded() {
    FakeClock clock;
    TimerEngine engine;
    CHECK(engine.startPomodoro({maxDurationMs, maxDurationMs, maxCycles},
                               clock.now, "maximum"));
    clock.elapse(2 * maxDurationMs * maxCycles);
    const auto events = engine.advance(clock.now);
    CHECK(events.size() == static_cast<std::size_t>(2 * maxCycles));
    CHECK(events.back().final && events.back().sequence == 2 * maxCycles);
    CHECK(engine.pomodoro.status == Status::Completed);
    CHECK(engine.advance(clock.now).empty());
}

void backwardClockRecoveryKeepsUtcRemaining() {
    constexpr Millis start = 1'700'000'000'000LL;
    constexpr Millis tenMinutes = 10 * 60'000;
    constexpr Millis oneHour = 60 * 60'000;
    TimerEngine initial;
    CHECK(initial.startPomodoro({tenMinutes, 5 * 60'000, 2}, start, "clock-back-pomo"));
    CHECK(initial.startCountdown(tenMinutes, start, "clock-back-countdown"));

    // A true exit loses the monotonic anchor. Recovery must use the persisted
    // UTC deadline and the newly adjusted wall clock, even beyond nominal time.
    TimerEngine restored = initial;
    const Millis returned = start - oneHour;
    CHECK(restored.advance(returned).empty());
    CHECK(restored.pomodoroRemaining(returned) == tenMinutes + oneHour);
    CHECK(restored.countdownRemaining(returned) == tenMinutes + oneHour);
    restored.pausePomodoro(returned);
    restored.pauseCountdown(returned);
    CHECK(restored.pomodoro.remainingMs == tenMinutes + oneHour);
    CHECK(restored.countdown.remainingMs == tenMinutes + oneHour);

    TimerEngine pausedRestart = restored;
    const Millis resumed = start + 24 * oneHour;
    CHECK(pausedRestart.pomodoroRemaining(resumed) == tenMinutes + oneHour);
    CHECK(pausedRestart.countdownRemaining(resumed) == tenMinutes + oneHour);
    pausedRestart.resumePomodoro(resumed);
    pausedRestart.resumeCountdown(resumed);
    CHECK(pausedRestart.advance(resumed + tenMinutes).empty());
    CHECK(pausedRestart.pomodoroRemaining(resumed + tenMinutes) == oneHour);
    CHECK(pausedRestart.countdownRemaining(resumed + tenMinutes) == oneHour);
    CHECK(pausedRestart.advance(resumed + tenMinutes + oneHour).size() == 2);
    CHECK(pausedRestart.pomodoro.phase == Phase::Break);
    CHECK(pausedRestart.pomodoroRemaining(resumed + tenMinutes + oneHour) == 5 * 60'000);
}

void randomizedCommandsMatchElapsedTimeModel() {
    // The reference models retain elapsed duration instead of UTC deadlines.
    // Fixed seed keeps failures reproducible across machines.
    std::mt19937 random(0x504f4d4f);
    const auto next = [&](unsigned maximum) { return random() % maximum; };
    struct ReferencePomodoro {
        Status status = Status::Idle;
        PomodoroConfig config;
        int phaseIndex = 0;
        Millis left = config.focusMs;
        std::uint64_t sequence = 0;
        Millis phaseDuration() const { return phaseIndex % 2 ? config.breakMs : config.focusMs; }
        void reset(PomodoroConfig value) {
            status = Status::Idle; config = value; phaseIndex = 0; left = config.focusMs; sequence = 0;
        }
        void finishPhase() {
            ++sequence;
            if (phaseIndex + 1 == 2 * config.cycles) { status = Status::Completed; left = 0; }
            else { ++phaseIndex; left = phaseDuration(); }
        }
    } pomo;
    struct ReferenceCountdown {
        Status status = Status::Idle;
        Millis duration = 300'000;
        Millis left = duration;
        std::uint64_t sequence = 0;
    } countdown;
    TimerEngine engine;
    Millis now = 1'700'000'000'000LL;
    for (int step = 0; step < 25'000; ++step) {
        const Millis elapsed = static_cast<Millis>(next(12'001));
        now += elapsed;
        std::vector<CompletionEvent> expected;
        if (pomo.status == Status::Running) {
            Millis pending = elapsed;
            while (pomo.status == Status::Running && pending >= pomo.left) {
                pending -= pomo.left;
                expected.push_back({TimerKind::Pomodoro,
                    pomo.phaseIndex % 2 ? Phase::Break : Phase::Focus,
                    pomo.phaseIndex / 2 + 1, pomo.sequence + 1,
                    pomo.phaseIndex + 1 == 2 * pomo.config.cycles});
                pomo.finishPhase();
            }
            if (pomo.status == Status::Running) pomo.left -= pending;
        }
        if (countdown.status == Status::Running) {
            if (elapsed >= countdown.left) {
                countdown.status = Status::Completed; countdown.left = 0; ++countdown.sequence;
                expected.push_back({TimerKind::Countdown, Phase::Focus, 1, countdown.sequence, true});
            } else countdown.left -= elapsed;
        }
        const auto actual = engine.advance(now);
        CHECK(actual.size() == expected.size());
        for (std::size_t i = 0; i < actual.size(); ++i) {
            CHECK(actual[i].timer == expected[i].timer);
            CHECK(actual[i].phase == expected[i].phase);
            CHECK(actual[i].cycle == expected[i].cycle);
            CHECK(actual[i].sequence == expected[i].sequence);
            CHECK(actual[i].final == expected[i].final);
        }
        CHECK(engine.advance(now).empty());
        const auto config = [&]() {
            return PomodoroConfig{static_cast<Millis>(next(50'000)) + 1,
                static_cast<Millis>(next(20'000)) + 1, static_cast<int>(next(8)) + 1};
        };
        switch (next(10)) {
        case 0: {
            const auto value = config();
            const bool permitted = pomo.status == Status::Idle || pomo.status == Status::Completed;
            CHECK(engine.startPomodoro(value, now, "property-pomo") == permitted);
            if (permitted) { pomo.reset(value); pomo.status = Status::Running; }
            break;
        }
        case 1:
            engine.pausePomodoro(now);
            if (pomo.status == Status::Running) pomo.status = Status::Paused;
            break;
        case 2:
            engine.resumePomodoro(now);
            if (pomo.status == Status::Paused) pomo.status = Status::Running;
            break;
        case 3:
            engine.skipPomodoro(now);
            if (pomo.status == Status::Running || pomo.status == Status::Paused) pomo.finishPhase();
            break;
        case 4: {
            const auto value = config(); engine.resetPomodoro(value); pomo.reset(value); break;
        }
        case 5: {
            const Millis value = static_cast<Millis>(next(50'000)) + 1;
            const bool permitted = countdown.status == Status::Idle || countdown.status == Status::Completed;
            CHECK(engine.startCountdown(value, now, "property-countdown") == permitted);
            if (permitted) { countdown = {}; countdown.duration = value; countdown.left = value; countdown.status = Status::Running; }
            break;
        }
        case 6:
            engine.pauseCountdown(now);
            if (countdown.status == Status::Running) countdown.status = Status::Paused;
            break;
        case 7:
            engine.resumeCountdown(now);
            if (countdown.status == Status::Paused) countdown.status = Status::Running;
            break;
        case 8: {
            const Millis value = static_cast<Millis>(next(50'000)) + 1;
            engine.resetCountdown(value); countdown = {}; countdown.duration = value; countdown.left = value; break;
        }
        case 9: {
            const Millis adjustment = static_cast<Millis>(next(7'200'001)) - 3'600'000;
            CHECK(engine.shiftDeadlines(adjustment)); now += adjustment; break;
        }
        }
        CHECK(engine.pomodoro.status == pomo.status);
        CHECK(engine.pomodoro.phase == (pomo.phaseIndex % 2 ? Phase::Break : Phase::Focus));
        CHECK(engine.pomodoro.cycle == pomo.phaseIndex / 2 + 1);
        CHECK(engine.pomodoro.config.focusMs == pomo.config.focusMs);
        CHECK(engine.pomodoro.config.breakMs == pomo.config.breakMs);
        CHECK(engine.pomodoro.config.cycles == pomo.config.cycles);
        CHECK(engine.pomodoro.phaseDurationMs == pomo.phaseDuration());
        CHECK(engine.pomodoro.transitionSequence == pomo.sequence);
        CHECK(engine.pomodoroRemaining(now) == pomo.left);
        CHECK(engine.pomodoro.deadlineUtcMs == (pomo.status == Status::Running ? now + pomo.left : 0));
        CHECK(engine.countdown.status == countdown.status);
        CHECK(engine.countdown.durationMs == countdown.duration);
        CHECK(engine.countdown.transitionSequence == countdown.sequence);
        CHECK(engine.countdownRemaining(now) == countdown.left);
        CHECK(engine.countdown.deadlineUtcMs == (countdown.status == Status::Running ? now + countdown.left : 0));
    }
}

} // namespace

int main() {
    try {
        finalBreakIsIncluded();
        customCyclesAndLateCallbacks();
        pausePersistsAcrossAbsence();
        trueExitKeepsDeadlines();
        timersAreIndependent();
        resetSkipAndCapturedConfiguration();
        invalidInputPreservesState();
        arithmeticBoundsAreChecked();
        wallClockAdjustmentsPreserveRemaining();
        maximumScheduleRecoveryIsBounded();
        backwardClockRecoveryKeepsUtcRemaining();
        randomizedCommandsMatchElapsedTimeModel();
        std::cout << "12 timer engine scenarios passed (including 25,000 randomized command steps)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
