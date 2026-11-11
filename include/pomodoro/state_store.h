#pragma once

#include "pomodoro/timer_engine.h"

#include <filesystem>
#include <string>

namespace pomodoro {

enum class BackgroundMode { TrayOnly, KeepTaskbar };
enum class TaskbarSource { Pomodoro, Countdown };

struct Preferences {
    PomodoroConfig pomodoro;
    Millis countdownDurationMs = 5 * 60 * 1000;
    bool soundEnabled = true;
    bool startAtSignIn = false;
    bool showTaskbarStatus = true;
    // Retained for settings-file compatibility; minimize now always uses tray.
    BackgroundMode backgroundMode = BackgroundMode::TrayOnly;
    TaskbarSource taskbarSource = TaskbarSource::Pomodoro;
};

struct AppState {
    PomodoroState pomodoro;
    CountdownState countdown;
    Millis savedAtUtcMs = 0;
    std::uint64_t pomodoroNotificationSequence = 0;
    std::uint64_t countdownNotificationSequence = 0;
};

template <typename T> struct LoadResult {
    T value{};
    bool recoveredFromBackup = false;
    std::string warning;
};

// Small, bounded, versioned UTF-8 JSON files. All failures are returned on load
// or thrown as std::runtime_error on save, so the controller can display them.
class StateStore {
public:
    explicit StateStore(std::filesystem::path directory);
    [[nodiscard]] const std::filesystem::path& directory() const noexcept;
    [[nodiscard]] LoadResult<AppState> loadState() const;
    [[nodiscard]] LoadResult<Preferences> loadPreferences() const;
    void saveState(const AppState& state) const;
    void savePreferences(const Preferences& preferences) const;

private:
    std::filesystem::path directory_;
};

} // namespace pomodoro
