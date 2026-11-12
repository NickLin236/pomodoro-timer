# Pomodoro Native

A small offline Windows 10 x64 desktop application in C++20 and Win32. It has independent Pomodoro and countdown timers, native controls, a notification-area menu, actionable local notifications, a taskbar status panel, opt-in sign-in startup, and restart recovery. There are no third-party runtime dependencies or network calls.

## Download and run

Download **Pomodoro.exe** from [the latest release](https://github.com/NickLin236/pomodoro-timer/releases/latest), save it in a permanent folder, and double-click it. You do not need to clone this repository, install development tools, or download any other application files. The executable creates its own settings and timer state in your Windows user profile.

[Download Pomodoro.exe](https://github.com/NickLin236/pomodoro-timer/releases/latest/download/Pomodoro.exe)

The latest-download link advances after a successful release workflow. Version **1.2.0** adds timer-specific notifications with Dismiss/Stop actions, countdown taskbar status, and broader taskbar detection. Releases contain the standalone executable, an optional ZIP with install scripts/documentation, and SHA-256 checksums. See the [changelog](CHANGELOG.md) and [release steps](Docs/GitHub_Releases.md).

## Build on Windows

Install Visual Studio 2022 with **Desktop development with C++**, a Windows 10 SDK, and CMake 3.21 or newer. In a developer PowerShell at the project root:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-release
ctest --preset windows-release
.\build\windows-msvc\Release\Pomodoro.exe
```

The Release build uses the static MSVC CRT (`/MT`). Icons and the application manifest are embedded. Builds and the resulting application require no dependency downloads. Development tools must already be installed.

Create an offline ZIP package after building:

```powershell
cpack --config build/windows-msvc/CPackConfig.cmake -C Release
```

Extract the ZIP and run `Install.ps1` from PowerShell to copy the executable to `%LOCALAPPDATA%\Programs\PomodoroNative` and create a Start Menu shortcut. It requires no elevation. Sign-in startup remains off until enabled in the app. `Uninstall.ps1` removes the installed executable and the shortcuts, startup entry, and notification protocol that still point to it; registrations owned by another portable copy are preserved. Timer data is preserved unless `-RemoveData` is supplied. Keep the extracted package for uninstall and measurement scripts.

You can also run the executable directly. Move it to its permanent location before enabling sign-in startup. Notifications register their own per-user identity, `pomodoro-native` protocol, and **Pomodoro Notifications** Start Menu shortcut pointing to that executable; no separate installer is needed. Run the moved executable again to refresh its notification registration. `Pomodoro.exe --background` starts hidden in the notification area. If Windows cannot create the notification icon, it retains a minimized taskbar button so the app remains accessible.

## Use

- **Pomodoro:** set focus/break minutes and cycles, then Start. One cycle includes focus and break, including the final break. Pause/Resume, Skip phase, and Reset are available. Save setup during a session to configure the next session; the active setup remains displayed.
- **Timer:** enter hours, minutes, and seconds. It runs independently of Pomodoro.
- **Settings:** choose sound, sign-in startup, **Show timer status in taskbar**, and the preferred taskbar timer when both timers are active. Save settings applies changes. Test notification requests a local Windows banner and displays any setup or policy error in the window.
- **Minimize:** hides the window in the notification area while timers keep running. Open from the notification icon, or launch another copy to activate the existing instance.
- **Close, Alt+F4, or Exit:** saves and terminates the application. Closing removes its notification icon and status panel. Running deadlines still advance logically and are reconciled on the next launch.
- **Taskbar status:** the active Pomodoro shows its phase, remaining time, and cycle position; the countdown shows its remaining time. The preferred source is used when both are active, with automatic fallback to the active timer. Paused sessions remain visible as paused. Reset and final completion hide the panel when neither timer is active. The panel appears immediately left of Traffic Monitor when detected, or left of the notification area otherwise. It targets the primary horizontal taskbar and draws over existing space; leave a gap on a crowded taskbar or disable it if it covers application buttons. Windows 11 layouts are attempted when usable anchors exist, but placement, auto-hide, and DPI behavior need a real Windows desktop check. Vertical taskbars are unsupported.
- **Notifications:** Pomodoro focus/break messages contain only Pomodoro details; countdown messages contain only timer details. **Dismiss** removes the notification from Windows notification history and keeps the session running. **Stop** resets the session that created that notification without stopping the other timer. An old notification cannot stop a newer session.

State and settings are stored separately as UTF-8 JSON in `%LOCALAPPDATA%\PomodoroNative`. Each file has a last-known-good `.bak`. Save/recovery errors remain visible in the window. Running timers advance while the app is exited; paused timers retain their remaining time. On return after missed boundaries, each affected timer gets one current summary instead of a sequence of old notifications.

Windows controls notification delivery and tray placement. Check Windows notification settings and Focus Assist/Do not disturb if banners are hidden; the application reports known setup, policy, and delivery failures. Rejected completion requests retry twice at five-second intervals while that session remains current. Windows can also report a later delivery failure, which appears in the application status. No notification can run while the process is exited or the PC is asleep/off. The next launch/resume reconciles time. A clock change while the process is exited affects UTC recovery. See [behavior](Docs/Behavior.md) for limits and exact semantics.

## Portable tests

The engine, JSON store, controller, and notification content/action checks build on Linux without the Windows UI:

```sh
cmake -S . -B build/portable -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/portable --parallel
ctest --test-dir build/portable --output-on-failure
```

With CMake 3.21+, `cmake --preset portable`, `cmake --build --preset portable`, and `ctest --preset portable` are equivalent. CMake 3.16+ supports the explicit commands (older CTest can use `cd build/portable` followed by `ctest --output-on-failure`). All dependencies are in this repository; no FetchContent, package restore, browser runtime, or service is required.

The portable suites include 25,000 deterministic randomized engine command steps and checks for timer recovery, durable storage, notification content/actions, and migration of the taskbar-status preference. Reset regressions cover the final-break → countdown → new Pomodoro sequence and reset exactly at a phase boundary. Windows builds add a `windows_ui` regression test that drives real native settings commands against an isolated registry key and checks that Close destroys the window after saving. It requires a Windows desktop and leaves the real startup entry untouched. Each CTest case has a 30-second timeout. The [validation record](Docs/Validation.md) distinguishes completed checks from the remaining Windows checks.

## Verification status

Portable Release tests, cross-build results, and the reported earlier GitHub CI result are recorded in [validation](Docs/Validation.md). The new version's Windows 10 status placement, Traffic Monitor coexistence, DPI interaction, PowerShell installation, and resource usage require the [Windows checklist](Docs/Windows_Test_Checklist.md). The provisional minimized private-byte target is 40 MiB; it is **not a measured result**. `Measure-Resources.ps1` records performance data rather than assuming that target has been met.

## Source layout

`Core` contains deterministic deadline arithmetic; `Persistence` owns validated JSON and durable file replacement; `App/Controller` handles clock anchoring, persistence, and notification attempts; `Platform` owns Windows shell/clock/startup APIs; `UI` owns native controls and the single message-loop scheduler. Tests inject `FakeClock`; real clock reads do not enter the engine.

The original [development plan](Pomodoro_Development_Plan.md) records the initial proposal. The current [behavior specification](Docs/Behavior.md) includes the later Close/Minimize, taskbar, and notification changes. Windows API references include [notification-area behavior](https://learn.microsoft.com/en-us/windows/win32/shell/notification-area), [notification actions](https://learn.microsoft.com/en-us/uwp/schemas/tiles/toastschema/element-action), [QueryInterruptTime](https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-queryinterrupttime), and [CMake static CRT selection](https://cmake.org/cmake/help/latest/prop_tgt/MSVC_RUNTIME_LIBRARY.html).
