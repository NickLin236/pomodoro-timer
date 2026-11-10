#include "pomodoro/controller.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace pomodoro;

namespace {

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string(__func__) + ":" + std::to_string(__LINE__) + ": " #condition); } while (false)

class TemporaryDirectory {
public:
    std::filesystem::path path;
    TemporaryDirectory() {
        static unsigned serial = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
            ("pomodoro-controller-tests-" + std::to_string(stamp) + "-" + std::to_string(++serial));
        if (!std::filesystem::create_directory(path)) throw std::runtime_error("Cannot create test directory");
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

FakeClock clockAtLaunch() { return FakeClock({1'700'000'000'000LL, 100'000}); }

void restartUsesPersistedUtcDeadline() {
    TemporaryDirectory files;
    StateStore store(files.path);
    auto clock = clockAtLaunch();
    Controller initial(store, clock);
    CHECK(initial.load().events.empty());
    CHECK(initial.startPomodoro({600'000, 300'000, 2}));
    CHECK(initial.startCountdown(600'000));
    clock.advance(300'000);
    clock.sample.monotonicMs = 0; // A reboot resets the monotonic epoch.
    Controller restored(store, clock);
    const auto update = restored.load();
    CHECK(update.recovery && update.events.empty());
    CHECK(restored.engine().pomodoroRemaining(restored.now()) == 300'000);
    CHECK(restored.engine().countdownRemaining(restored.now()) == 300'000);
    CHECK(restored.running());
    CHECK(restored.warning().empty());
}

void backwardClockWhileExitedPreservesUtcRecoveryAndPause() {
    TemporaryDirectory files;
    StateStore store(files.path);
    auto clock = clockAtLaunch();
    Controller initial(store, clock);
    (void)initial.load();
    CHECK(initial.startPomodoro({600'000, 300'000, 2}));
    CHECK(initial.startCountdown(600'000));

    // Unlike an in-process clock adjustment, a true exit has only its saved
    // UTC deadline. Moving UTC back an hour must expose the full 70 minutes.
    clock.adjustWallClock(-3'600'000);
    clock.sample.monotonicMs = 0;
    Controller restored(store, clock);
    CHECK(restored.load().events.empty());
    CHECK(restored.engine().pomodoroRemaining(restored.now()) == 4'200'000);
    CHECK(restored.engine().countdownRemaining(restored.now()) == 4'200'000);
    restored.togglePomodoro();
    restored.toggleCountdown();
    CHECK(restored.warning().empty());

    clock.advance(5'000'000);
    clock.sample.monotonicMs = 0;
    Controller paused(store, clock);
    CHECK(paused.load().events.empty());
    CHECK(paused.engine().pomodoro.status == Status::Paused);
    CHECK(paused.engine().countdown.status == Status::Paused);
    CHECK(paused.engine().pomodoroRemaining(paused.now()) == 4'200'000);
    CHECK(paused.engine().countdownRemaining(paused.now()) == 4'200'000);
    paused.togglePomodoro();
    paused.toggleCountdown();
    CHECK(paused.warning().empty());
    clock.advance(4'200'000);
    const auto events = paused.poll();
    CHECK(events.events.size() == 2);
    CHECK(paused.engine().pomodoro.phase == Phase::Break);
    CHECK(paused.engine().countdown.status == Status::Completed);
}

void recoveryReconcilesAndDeduplicates() {
    TemporaryDirectory files;
    StateStore store(files.path);
    auto clock = clockAtLaunch();
    Controller initial(store, clock);
    (void)initial.load();
    CHECK(initial.startPomodoro({120'000, 60'000, 3}));
    clock.advance(240'000);
    Controller restored(store, clock);
    const auto recovered = restored.load();
    CHECK(recovered.recovery && recovered.events.size() == 2);
    CHECK(restored.engine().pomodoro.phase == Phase::Focus);
    CHECK(restored.engine().pomodoro.cycle == 2);
    CHECK(restored.engine().pomodoroRemaining(restored.now()) == 60'000);
    const auto saved = store.loadState();
    CHECK(saved.warning.empty());
    CHECK(saved.value.pomodoroNotificationSequence == 2);
    CHECK(saved.value.pomodoro.transitionSequence == 2);
    Controller secondRestart(store, clock);
    CHECK(secondRestart.load().events.empty());
    CHECK(secondRestart.poll().events.empty());
}

void normalEventsArePersistedBeforeDelivery() {
    TemporaryDirectory files;
    StateStore store(files.path);
    auto clock = clockAtLaunch();
    Controller controller(store, clock);
    (void)controller.load();
    CHECK(controller.startPomodoro({1'000, 1'000, 1}));
    CHECK(controller.startCountdown(1'000));
    clock.advance(1'000);
    const auto update = controller.poll();
    CHECK(!update.recovery && update.events.size() == 2);
    const auto stateAtDelivery = store.loadState().value;
    CHECK(stateAtDelivery.pomodoroNotificationSequence == 1);
    CHECK(stateAtDelivery.countdownNotificationSequence == 1);
    CHECK(stateAtDelivery.countdown.status == Status::Completed);
    CHECK(controller.poll().events.empty());
    Controller restarted(store, clock);
    CHECK(restarted.load().events.empty());
    clock.advance(1'000);
    const auto final = restarted.poll();
    CHECK(!final.recovery && final.events.size() == 1);
    CHECK(final.events[0].timer == TimerKind::Pomodoro && final.events[0].final);
    CHECK(final.events[0].phase == Phase::Break && final.events[0].sequence == 2);
    Controller afterFinal(store, clock);
    CHECK(afterFinal.load().events.empty());
    CHECK(!afterFinal.running());
}

void pausedTimersFreezeAcrossReboot() {
    TemporaryDirectory files;
    StateStore store(files.path);
    auto clock = clockAtLaunch();
    Controller controller(store, clock);
    (void)controller.load();
    CHECK(controller.startPomodoro({600'000, 300'000, 2}));
    CHECK(controller.startCountdown(600'000));
    clock.advance(1'234);
    CHECK(controller.poll().events.empty());
    controller.togglePomodoro();
    controller.toggleCountdown();
    CHECK(!controller.running());
    CHECK(controller.engine().pomodoro.remainingMs == 598'766);
    clock.advance(30LL * 24 * 60 * 60 * 1000);
    clock.sample.monotonicMs = 0;
    Controller restored(store, clock);
    CHECK(restored.load().events.empty());
    CHECK(restored.engine().pomodoro.status == Status::Paused);
    CHECK(restored.engine().countdown.status == Status::Paused);
    CHECK(restored.engine().pomodoroRemaining(restored.now()) == 598'766);
    CHECK(restored.engine().countdownRemaining(restored.now()) == 598'766);
    restored.togglePomodoro();
    restored.toggleCountdown();
    CHECK(restored.running());
    clock.advance(598'766);
    const auto elapsed = restored.poll();
    CHECK(elapsed.recovery && elapsed.events.size() == 2);
    CHECK(restored.engine().pomodoro.phase == Phase::Break);
    CHECK(restored.engine().countdown.status == Status::Completed);
}

void wallClockChangesPreserveMonotonicRemaining() {
    TemporaryDirectory files;
    StateStore store(files.path);
    auto clock = clockAtLaunch();
    Controller controller(store, clock);
    (void)controller.load();
    CHECK(controller.startPomodoro({600'000, 120'000, 2}));
    CHECK(controller.startCountdown(900'000));
    clock.advance(100'000);
    CHECK(controller.poll().events.empty());
    const Millis originalDeadline = controller.engine().pomodoro.deadlineUtcMs;
    clock.adjustWallClock(3'600'000);
    CHECK(controller.poll().events.empty());
    CHECK(controller.engine().pomodoroRemaining(controller.now()) == 500'000);
    CHECK(controller.engine().countdownRemaining(controller.now()) == 800'000);
    CHECK(controller.engine().pomodoro.deadlineUtcMs == originalDeadline + 3'600'000);
    CHECK(store.loadState().value.pomodoro.deadlineUtcMs == originalDeadline + 3'600'000);
    clock.adjustWallClock(-7'200'000);
    CHECK(controller.poll().events.empty());
    CHECK(controller.engine().pomodoroRemaining(controller.now()) == 500'000);
    CHECK(controller.engine().countdownRemaining(controller.now()) == 800'000);
    controller.togglePomodoro();
    clock.adjustWallClock(3'600'000);
    CHECK(controller.poll().events.empty());
    CHECK(controller.engine().pomodoro.status == Status::Paused);
    CHECK(controller.engine().pomodoroRemaining(controller.now()) == 500'000);
    Controller restarted(store, clock);
    CHECK(restarted.load().events.empty());
    CHECK(restarted.engine().countdownRemaining(restarted.now()) == 800'000);
    CHECK(restarted.engine().pomodoroRemaining(restarted.now()) == 500'000);
}

void wallClockJitterDoesNotChangeElapsedTiming() {
    TemporaryDirectory files;
    StateStore store(files.path);
    auto clock = clockAtLaunch();
    Controller controller(store, clock);
    (void)controller.load();
    CHECK(controller.startCountdown(10'000));
    const Millis originalDeadline = controller.engine().countdown.deadlineUtcMs;
    clock.adjustWallClock(500);
    clock.advance(1'000);
    CHECK(controller.poll().events.empty());
    CHECK(controller.engine().countdownRemaining(controller.now()) == 9'000);
    clock.adjustWallClock(600);
    CHECK(controller.poll().events.empty());
    CHECK(controller.engine().countdownRemaining(controller.now()) == 9'000);
    CHECK(controller.engine().countdown.deadlineUtcMs == originalDeadline + 1'100);
}

void suspendAndLongAbsenceProduceOneRecoveryBatch() {
    TemporaryDirectory files;
    StateStore store(files.path);
    auto clock = clockAtLaunch();
    Controller controller(store, clock);
    (void)controller.load();
    CHECK(controller.startPomodoro({1'000, 1'000, 2}));
    CHECK(controller.startCountdown(5'000));
    clock.advance(10'000); // QueryInterruptTime includes suspended time.
    const auto missed = controller.poll();
    CHECK(missed.recovery && missed.events.size() == 5);
    CHECK(controller.engine().pomodoro.status == Status::Completed);
    CHECK(controller.engine().countdown.status == Status::Completed);
    CHECK(!controller.running());
    CHECK(controller.poll().events.empty());
    Controller restarted(store, clock);
    CHECK(restarted.load().events.empty());
}

void preferenceEditsApplyToTheNextSession() {
    TemporaryDirectory files;
    StateStore store(files.path);
    auto clock = clockAtLaunch();
    Controller controller(store, clock);
    (void)controller.load();
    CHECK(controller.startPomodoro({6'000, 2'000, 2}));
    CHECK(controller.startCountdown(5'000));
    auto preferences = controller.preferences();
    preferences.pomodoro = {9'000, 3'000, 3};
    preferences.countdownDurationMs = 8'000;
    preferences.backgroundMode = BackgroundMode::KeepTaskbar;
    preferences.taskbarSource = TaskbarSource::Countdown;
    preferences.soundEnabled = false;
    CHECK(controller.savePreferences(preferences));
    CHECK(controller.engine().pomodoro.config.focusMs == 6'000);
    CHECK(controller.engine().pomodoro.config.cycles == 2);
    CHECK(controller.engine().countdown.durationMs == 5'000);
    auto invalid = preferences;
    invalid.pomodoro.cycles = 0;
    CHECK(!controller.savePreferences(invalid));
    CHECK(controller.preferences().pomodoro.cycles == 3);
    CHECK(controller.engine().pomodoro.config.cycles == 2);
    controller.resetPomodoro();
    controller.resetCountdown();
    CHECK(controller.engine().pomodoroRemaining(controller.now()) == 9'000);
    CHECK(controller.engine().countdownRemaining(controller.now()) == 8'000);
    CHECK(controller.engine().pomodoro.status == Status::Idle);
    Controller restarted(store, clock);
    CHECK(restarted.load().events.empty());
    CHECK(restarted.preferences().backgroundMode == BackgroundMode::KeepTaskbar);
    CHECK(restarted.preferences().taskbarSource == TaskbarSource::Countdown);
    CHECK(!restarted.preferences().soundEnabled);
    CHECK(restarted.engine().pomodoroRemaining(restarted.now()) == 9'000);
}

void skipPersistsWithoutCompletionNotification() {
    TemporaryDirectory files;
    StateStore store(files.path);
    auto clock = clockAtLaunch();
    Controller controller(store, clock);
    (void)controller.load();
    CHECK(controller.startPomodoro({2'000, 1'000, 1}));
    controller.skipPomodoro();
    CHECK(controller.poll().events.empty());
    CHECK(store.loadState().value.pomodoro.transitionSequence == 1);
    Controller restarted(store, clock);
    CHECK(restarted.load().events.empty());
    CHECK(restarted.engine().pomodoro.phase == Phase::Break);
    clock.advance(1'000);
    const auto completion = restarted.poll();
    CHECK(completion.events.size() == 1 && completion.events[0].sequence == 2);
    CHECK(completion.events[0].final);
}

void saveErrorsRemainVisibleAndAreRecoverable() {
    TemporaryDirectory files;
    const auto blocked = files.path / "blocked";
    { std::ofstream output(blocked); output << "a file cannot contain state documents"; }
    StateStore store(blocked);
    auto clock = clockAtLaunch();
    Controller controller(store, clock);
    (void)controller.load();
    controller.clearWarning();
    CHECK(controller.startCountdown(60'000));
    CHECK(controller.running());
    CHECK(!controller.warning().empty());
    CHECK(controller.warning().find("Could not save") != std::string::npos);
    CHECK(!controller.save());
    auto changed = controller.preferences();
    changed.soundEnabled = !changed.soundEnabled;
    CHECK(!controller.savePreferences(changed));
    CHECK(controller.preferences().soundEnabled != changed.soundEnabled);
    CHECK(std::filesystem::remove(blocked));
    CHECK(controller.save());
    controller.clearWarning();
    CHECK(controller.warning().empty());
    Controller restarted(store, clock);
    CHECK(restarted.load().events.empty());
    CHECK(restarted.engine().countdown.status == Status::Running);
    CHECK(restarted.engine().countdownRemaining(restarted.now()) == 60'000);
}

void idlePollsAndDisplayUpdatesDoNotRewriteState() {
    TemporaryDirectory files;
    StateStore store(files.path);
    auto clock = clockAtLaunch();
    Controller controller(store, clock);
    (void)controller.load();
    CHECK(!std::filesystem::exists(files.path / "state.json"));
    clock.advance(1'000);
    CHECK(controller.poll().events.empty());
    CHECK(!std::filesystem::exists(files.path / "state.json"));
    CHECK(controller.startCountdown(60'000));
    const auto savedAt = store.loadState().value.savedAtUtcMs;
    for (int tick = 0; tick < 5; ++tick) {
        clock.advance(1'000);
        CHECK(controller.poll().events.empty());
    }
    CHECK(store.loadState().value.savedAtUtcMs == savedAt);
    controller.toggleCountdown();
    CHECK(store.loadState().value.savedAtUtcMs == controller.now());
    CHECK(!controller.running());
}

void commandsUseOneSampledClockTransaction() {
    class CountingClock final : public Clock {
    public:
        mutable int reads = 0;
        ClockSample sample{1'700'000'000'000LL, 0};
        ClockSample now() const override { ++reads; return sample; }
    } clock;
    TemporaryDirectory files;
    StateStore store(files.path);
    Controller controller(store, clock);
    (void)controller.load();
    CHECK(clock.reads == 1);
    CHECK(controller.startPomodoro({5'000, 1'000, 1}));
    CHECK(controller.startCountdown(10'000));
    CHECK(clock.reads == 1);
    clock.sample.utcMs += 1'000;
    clock.sample.monotonicMs += 1'000;
    CHECK(controller.poll().events.empty());
    CHECK(clock.reads == 2);
    controller.togglePomodoro();
    controller.toggleCountdown();
    controller.skipPomodoro();
    controller.resetPomodoro();
    controller.resetCountdown();
    CHECK(controller.savePreferences(controller.preferences()));
    CHECK(controller.save());
    CHECK(clock.reads == 2);
}

} // namespace

int main() {
    try {
        restartUsesPersistedUtcDeadline();
        backwardClockWhileExitedPreservesUtcRecoveryAndPause();
        recoveryReconcilesAndDeduplicates();
        normalEventsArePersistedBeforeDelivery();
        pausedTimersFreezeAcrossReboot();
        wallClockChangesPreserveMonotonicRemaining();
        wallClockJitterDoesNotChangeElapsedTiming();
        suspendAndLongAbsenceProduceOneRecoveryBatch();
        preferenceEditsApplyToTheNextSession();
        skipPersistsWithoutCompletionNotification();
        saveErrorsRemainVisibleAndAreRecoverable();
        idlePollsAndDisplayUpdatesDoNotRewriteState();
        commandsUseOneSampledClockTransaction();
        std::cout << "13 controller scenarios passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
