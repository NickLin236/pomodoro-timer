#include "pomodoro/state_store.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <locale>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace pomodoro;

namespace {

void check(bool condition, const char* expression, int line) {
    if (!condition) throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

struct Fixture {
    std::filesystem::path path;
    StateStore store;
    Fixture() : path(std::filesystem::temp_directory_path() /
                     ("pomodoro-state-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))),
                store(path) { std::filesystem::create_directories(path); }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(path, error); }
};

void write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
    if (!file) throw std::runtime_error("test fixture write failed");
}
std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
std::string replace(std::string value, const std::string& old, const std::string& replacement) {
    const auto position = value.find(old);
    CHECK(position != std::string::npos);
    value.replace(position, old.size(), replacement);
    return value;
}

AppState snapshot(const TimerEngine& engine, Millis now) {
    AppState result;
    result.pomodoro = engine.pomodoro;
    result.countdown = engine.countdown;
    result.savedAtUtcMs = now;
    return result;
}
TimerEngine restore(const AppState& state) {
    TimerEngine engine;
    engine.pomodoro = state.pomodoro;
    engine.countdown = state.countdown;
    return engine;
}

void staleTemporaryFilesDoNotPreventSave() {
    Fixture fixture;
#ifdef _WIN32
    const auto process = GetCurrentProcessId();
#else
    const auto process = getpid();
#endif
    // A crash can leave a temporary file behind. Recycled process IDs must not
    // turn those old names into a failed save or delete a file we did not open.
    for (int serial = 1; serial <= 4; ++serial)
        write(fixture.path / ("settings.json.bak.tmp-" + std::to_string(process) + "-" + std::to_string(serial)), "existing temporary file");
    fixture.store.savePreferences(Preferences{});
    CHECK(fixture.store.loadPreferences().warning.empty());
    for (int serial = 1; serial <= 4; ++serial)
        CHECK(read(fixture.path / ("settings.json.bak.tmp-" + std::to_string(process) + "-" + std::to_string(serial))) == "existing temporary file");
}

void numericSerializationIgnoresUserLocale() {
    struct GroupedNumbers : std::numpunct<char> {
        char do_thousands_sep() const override { return ','; }
        std::string do_grouping() const override { return "\3"; }
    };
    struct RestoreLocale {
        std::locale old = std::locale();
        ~RestoreLocale() { std::locale::global(old); }
    } restoreLocale;
    std::locale::global(std::locale(std::locale::classic(), new GroupedNumbers));
    Fixture fixture;
    AppState state;
    state.savedAtUtcMs = 1700000000000LL;
    fixture.store.saveState(state);
    CHECK(read(fixture.path / "state.json").find("\"savedAtUtcMs\":1700000000000") != std::string::npos);
    CHECK(fixture.store.loadState().value.savedAtUtcMs == state.savedAtUtcMs);
}

void backwardClockRecoveryCanBePausedAndSaved() {
    Fixture fixture;
    constexpr Millis start = 1700000000000LL;
    constexpr Millis duration = 10 * 60000;
    constexpr Millis returnedAt = start - 60 * 60000;
    TimerEngine engine;
    CHECK(engine.startPomodoro({duration, 5 * 60000, 2}, start, "p-backward-clock"));
    CHECK(engine.startCountdown(duration, start, "c-backward-clock"));
    fixture.store.saveState(snapshot(engine, start));
    auto returned = restore(fixture.store.loadState().value);
    CHECK(returned.pomodoroRemaining(returnedAt) == 70 * 60000);
    CHECK(returned.countdownRemaining(returnedAt) == 70 * 60000);
    returned.pausePomodoro(returnedAt);
    returned.pauseCountdown(returnedAt);
    fixture.store.saveState(snapshot(returned, returnedAt));
    auto paused = restore(fixture.store.loadState().value);
    CHECK(paused.pomodoro.status == Status::Paused);
    CHECK(paused.countdown.status == Status::Paused);
    CHECK(paused.pomodoroRemaining(start + 24 * 60 * 60000) == 70 * 60000);
    CHECK(paused.countdownRemaining(start + 24 * 60 * 60000) == 70 * 60000);
    paused.resumePomodoro(start);
    paused.resumeCountdown(start);
    fixture.store.saveState(snapshot(paused, start));
    CHECK(fixture.store.loadState().warning.empty());
    CHECK(fixture.store.loadState().value.pomodoro.deadlineUtcMs == start + 70 * 60000);
    CHECK(fixture.store.loadState().value.countdown.deadlineUtcMs == start + 70 * 60000);
    CHECK(paused.pomodoro.phaseDurationMs == duration);
    CHECK(paused.countdown.durationMs == duration);
}

void missingAndRoundTrips() {
    Fixture fixture;
    const auto emptyState = fixture.store.loadState();
    const auto emptyPreferences = fixture.store.loadPreferences();
    CHECK(emptyState.warning.empty());
    CHECK(!emptyState.recoveredFromBackup);
    CHECK(emptyState.value.pomodoro.status == Status::Idle);
    CHECK(emptyPreferences.warning.empty());
    CHECK(emptyPreferences.value.pomodoro.focusMs == 25 * 60000);

    Preferences preferences;
    preferences.pomodoro = {12345, 6789, 11};
    preferences.countdownDurationMs = 12345678;
    preferences.soundEnabled = false;
    preferences.startAtSignIn = true;
    preferences.backgroundMode = BackgroundMode::KeepTaskbar;
    preferences.taskbarSource = TaskbarSource::Countdown;
    fixture.store.savePreferences(preferences);
    const auto loaded = fixture.store.loadPreferences();
    CHECK(loaded.warning.empty());
    CHECK(loaded.value.pomodoro.focusMs == 12345);
    CHECK(loaded.value.pomodoro.breakMs == 6789);
    CHECK(loaded.value.pomodoro.cycles == 11);
    CHECK(loaded.value.countdownDurationMs == 12345678);
    CHECK(!loaded.value.soundEnabled);
    CHECK(loaded.value.startAtSignIn);
    CHECK(loaded.value.backgroundMode == BackgroundMode::KeepTaskbar);
    CHECK(loaded.value.taskbarSource == TaskbarSource::Countdown);

    TimerEngine engine;
    CHECK(engine.startPomodoro(preferences.pomodoro, 1000000, "session-\"\\-\xf0\x9f\x8d\x85"));
    CHECK(engine.startCountdown(1234, 1000000, "countdown-1"));
    CHECK(engine.advance(1001300).size() == 1);
    auto state = snapshot(engine, 1001300);
    state.countdownNotificationSequence = 1;
    fixture.store.saveState(state);
    const auto recovered = fixture.store.loadState();
    CHECK(recovered.warning.empty());
    CHECK(recovered.value.savedAtUtcMs == 1001300);
    CHECK(recovered.value.pomodoro.sessionId == state.pomodoro.sessionId);
    CHECK(recovered.value.pomodoro.config.focusMs == preferences.pomodoro.focusMs);
    CHECK(recovered.value.pomodoro.deadlineUtcMs == state.pomodoro.deadlineUtcMs);
    CHECK(recovered.value.countdown.status == Status::Completed);
    CHECK(recovered.value.countdownNotificationSequence == 1);
    CHECK(recovered.value.countdown.transitionSequence == 1);
}

void restartRecoveryAndPause() {
    Fixture fixture;
    constexpr Millis start = 1700000000000LL;
    TimerEngine engine;
    CHECK(engine.startPomodoro({120000, 60000, 3}, start, "p"));
    CHECK(engine.startCountdown(10 * 60000, start, "c"));
    fixture.store.saveState(snapshot(engine, start));
    auto returned = restore(fixture.store.loadState().value);
    CHECK(returned.countdownRemaining(start + 5 * 60000) == 5 * 60000);
    const auto events = returned.advance(start + 4 * 60000);
    CHECK(events.size() == 2);
    CHECK(returned.pomodoro.phase == Phase::Focus);
    CHECK(returned.pomodoro.cycle == 2);
    CHECK(returned.pomodoroRemaining(start + 4 * 60000) == 60000);

    returned.pausePomodoro(start + 4 * 60000);
    returned.pauseCountdown(start + 4 * 60000);
    fixture.store.saveState(snapshot(returned, start + 4 * 60000));
    auto paused = restore(fixture.store.loadState().value);
    CHECK(paused.advance(start + 100 * 24 * 60 * 60000LL).empty());
    CHECK(paused.pomodoroRemaining(start + 100 * 24 * 60 * 60000LL) == 60000);
    CHECK(paused.countdownRemaining(start + 100 * 24 * 60 * 60000LL) == 6 * 60000);
    paused.resumePomodoro(start + 100 * 24 * 60 * 60000LL);
    fixture.store.saveState(snapshot(paused, start + 100 * 24 * 60 * 60000LL));
    CHECK(fixture.store.loadState().value.pomodoro.status == Status::Running);

    // Completion identities survive a restart and reprocessing yields no event.
    (void)paused.advance(std::numeric_limits<Millis>::max());
    auto completed = snapshot(paused, start);
    completed.pomodoroNotificationSequence = completed.pomodoro.transitionSequence;
    fixture.store.saveState(completed);
    auto secondReturn = restore(fixture.store.loadState().value);
    CHECK(secondReturn.advance(std::numeric_limits<Millis>::max()).empty());
    CHECK(secondReturn.pomodoro.status == Status::Completed);
}

void backupRecovery() {
    Fixture fixture;
    Preferences first;
    first.pomodoro.cycles = 8;
    fixture.store.savePreferences(first);
    CHECK(std::filesystem::exists(fixture.path / "settings.json.bak"));
    Preferences second = first;
    second.pomodoro.cycles = 9;
    fixture.store.savePreferences(second);
    write(fixture.path / "settings.json", "{broken");
    auto loaded = fixture.store.loadPreferences();
    CHECK(loaded.recoveredFromBackup);
    CHECK(!loaded.warning.empty());
    CHECK(loaded.value.pomodoro.cycles == 8);

    // A subsequent save must not back up the corrupt primary over the good copy.
    fixture.store.savePreferences(second);
    write(fixture.path / "settings.json", "still corrupt");
    loaded = fixture.store.loadPreferences();
    CHECK(loaded.recoveredFromBackup);
    CHECK(loaded.value.pomodoro.cycles == 8);
    write(fixture.path / "settings.json.bak", "also corrupt");
    loaded = fixture.store.loadPreferences();
    CHECK(!loaded.recoveredFromBackup);
    CHECK(!loaded.warning.empty());
    CHECK(loaded.value.pomodoro.cycles == 4);

    fixture.store.saveState(AppState{});
    std::filesystem::remove(fixture.path / "state.json");
    const auto missingPrimary = fixture.store.loadState();
    CHECK(missingPrimary.recoveredFromBackup);
    CHECK(!missingPrimary.warning.empty());
    CHECK(missingPrimary.value.pomodoro.status == Status::Idle);
    CHECK(missingPrimary.value.pomodoro.config.focusMs == 25 * 60000);

    // A failed backup replacement must leave the current valid primary intact.
    fixture.store.savePreferences(second);
    const auto validPrimary = read(fixture.path / "settings.json");
    std::filesystem::remove(fixture.path / "settings.json.bak");
    std::filesystem::create_directory(fixture.path / "settings.json.bak");
    Preferences third = second;
    third.pomodoro.cycles = 10;
    bool threw = false;
    try { fixture.store.savePreferences(third); }
    catch (const std::exception&) { threw = true; }
    CHECK(threw);
    CHECK(read(fixture.path / "settings.json") == validPrimary);
    CHECK(fixture.store.loadPreferences().warning.empty());
}

void malformedAndInvalidDocuments() {
    Fixture fixture;
    fixture.store.saveState(AppState{});
    const auto validState = read(fixture.path / "state.json");
    std::filesystem::remove(fixture.path / "state.json.bak");
    const std::string invalidStates[] = {
        "", "null", "[]", "{}", "{\"version\":1,}",
        replace(validState, "\"version\":1", "\"version\":0"),
        replace(validState, "\"version\":1", "\"version\":2"),
        replace(validState, "\"version\":1", "\"version\":1,\"version\":1"),
        replace(validState, "\"cycles\":4", "\"cycles\":0"),
        replace(validState, "\"cycles\":4", "\"cycles\":1001"),
        replace(validState, "\"cycles\":4", "\"cycles\":4.0"),
        replace(validState, "\"cycles\":4", "\"cycles\":04"),
        replace(validState, "\"cycles\":4", "\"cycles\":999999999999999999999999999"),
        replace(validState, "\"status\":\"idle\"", "\"status\":\"unknown\""),
        replace(validState, "\"remainingMs\":1500000", "\"remainingMs\":-1"),
        replace(validState, "\"phaseDurationMs\":1500000", "\"phaseDurationMs\":1"),
        replace(validState, "\"phase\":\"focus\"", "\"phase\":\"unknown\""),
        replace(validState, "\"transitionSequence\":0", "\"transitionSequence\":1"),
        replace(validState, "\"savedAtUtcMs\":0", "\"savedAtUtcMs\":-1"),
        replace(validState, "\"pomodoroNotificationSequence\":0", "\"pomodoroNotificationSequence\":1"),
        replace(validState, "\"sessionId\":\"\"", "\"sessionId\":\"\\ud800\""),
        replace(validState, "\"sessionId\":\"\"", "\"sessionId\":\"\\u0000\""),
        validState + " trailing",
        std::string(65537, ' '),
        std::string(30, '[') + std::string(30, ']'),
        std::string("{\"version\":\"") + static_cast<char>(0xc0) + static_cast<char>(0x80) + "\"}"
    };
    for (const auto& document : invalidStates) {
        write(fixture.path / "state.json", document);
        const auto loaded = fixture.store.loadState();
        CHECK(!loaded.warning.empty());
        CHECK(!loaded.recoveredFromBackup);
        CHECK(loaded.value.pomodoro.status == Status::Idle);
    }

    fixture.store.savePreferences(Preferences{});
    const auto validPreferences = read(fixture.path / "settings.json");
    std::filesystem::remove(fixture.path / "settings.json.bak");
    const std::string invalidPreferences[] = {
        replace(validPreferences, "\"soundEnabled\":true", "\"soundEnabled\":1"),
        replace(validPreferences, "\"countdownDurationMs\":300000", "\"countdownDurationMs\":0"),
        replace(validPreferences, "\"backgroundMode\":\"trayOnly\"", "\"backgroundMode\":\"invalid\""),
        replace(validPreferences, "\"taskbarSource\":\"pomodoro\"", "\"taskbarSource\":\"invalid\"")
    };
    for (const auto& document : invalidPreferences) {
        write(fixture.path / "settings.json", document);
        CHECK(!fixture.store.loadPreferences().warning.empty());
    }
}

void schemaInvariantsAndNumericBounds() {
    Fixture fixture;
    TimerEngine engine;
    CHECK(engine.startPomodoro({maxDurationMs, maxDurationMs, maxCycles}, 1000000, "maximal-p"));
    CHECK(engine.startCountdown(maxDurationMs, std::numeric_limits<Millis>::max() - maxDurationMs, "maximal-c"));
    fixture.store.saveState(snapshot(engine, 1000000));
    const auto valid = read(fixture.path / "state.json");
    CHECK(fixture.store.loadState().warning.empty());
    CHECK(fixture.store.loadState().value.countdown.deadlineUtcMs == std::numeric_limits<Millis>::max());
    std::filesystem::remove(fixture.path / "state.json.bak");
    const auto overflowingSchedule = replace(valid, "\"deadlineUtcMs\":" + std::to_string(engine.pomodoro.deadlineUtcMs),
                                             "\"deadlineUtcMs\":" + std::to_string(std::numeric_limits<Millis>::max()));
    write(fixture.path / "state.json", overflowingSchedule);
    CHECK(!fixture.store.loadState().warning.empty());

    engine.resetPomodoro({1000, 500, 2});
    engine.resetCountdown(1000);
    CHECK(engine.startPomodoro({1000, 500, 2}, 1000000, "skip-p"));
    engine.pausePomodoro(1000500);
    engine.skipPomodoro(1000500);
    CHECK(engine.pomodoro.phase == Phase::Break);
    CHECK(engine.pomodoro.status == Status::Paused);
    fixture.store.saveState(snapshot(engine, 1000500));
    CHECK(fixture.store.loadState().warning.empty());
    const auto paused = read(fixture.path / "state.json");
    std::filesystem::remove(fixture.path / "state.json.bak");
    write(fixture.path / "state.json", replace(paused, "\"transitionSequence\":1", "\"transitionSequence\":0"));
    CHECK(!fixture.store.loadState().warning.empty());

    CHECK(engine.startCountdown(1000, 1000000, "c"));
    (void)engine.advance(1001000);
    fixture.store.saveState(snapshot(engine, 1001000));
    const auto completed = read(fixture.path / "state.json");
    std::filesystem::remove(fixture.path / "state.json.bak");
    const auto countdownPosition = completed.find("\"countdown\"");
    CHECK(countdownPosition != std::string::npos);
    const auto invalidCountdown = completed.substr(0, countdownPosition) +
        replace(completed.substr(countdownPosition), "\"transitionSequence\":1", "\"transitionSequence\":0");
    write(fixture.path / "state.json", invalidCountdown);
    CHECK(!fixture.store.loadState().warning.empty());
}

void invalidSavesAndIoFailures() {
    Fixture fixture;
    fixture.store.saveState(AppState{});
    const auto original = read(fixture.path / "state.json");
    AppState invalidState;
    invalidState.pomodoro.config.cycles = 0;
    bool threw = false;
    try { fixture.store.saveState(invalidState); }
    catch (const std::exception&) { threw = true; }
    CHECK(threw);
    CHECK(read(fixture.path / "state.json") == original);
    Preferences invalidPreferences;
    invalidPreferences.countdownDurationMs = maxDurationMs + 1;
    threw = false;
    try { fixture.store.savePreferences(invalidPreferences); }
    catch (const std::exception&) { threw = true; }
    CHECK(threw);

    const auto notDirectory = fixture.path / "not-a-directory";
    write(notDirectory, "file");
    StateStore badStore(notDirectory);
    CHECK(!badStore.loadState().warning.empty());
    CHECK(!badStore.loadPreferences().warning.empty());
    threw = false;
    try { badStore.saveState(AppState{}); }
    catch (const std::exception& error) {
        threw = std::string(error.what()).find("Could not save state.json") != std::string::npos;
    }
    CHECK(threw);
}

} // namespace

int main() {
    try {
        staleTemporaryFilesDoNotPreventSave();
        numericSerializationIgnoresUserLocale();
        missingAndRoundTrips();
        restartRecoveryAndPause();
        backwardClockRecoveryCanBePausedAndSaved();
        backupRecovery();
        malformedAndInvalidDocuments();
        schemaInvariantsAndNumericBounds();
        invalidSavesAndIoFailures();
        std::cout << "State store tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
