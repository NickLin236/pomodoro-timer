# Pomodoro Native

A small offline Windows 10 x64 desktop application in C++20 and Win32. It has independent Pomodoro and countdown timers, native controls, a tray menu, local notifications, taskbar progress, opt-in sign-in startup, and restart recovery. There are no third-party runtime dependencies or network calls.

## Download and run

Download **Pomodoro.exe** from [the latest release](https://github.com/NickLin236/pomodoro-timer/releases/latest), save it in a permanent folder, and double-click it. You do not need to clone this repository, install development tools, or download any other application files. The executable creates its own settings and timer state in your Windows user profile.

[Download Pomodoro.exe](https://github.com/NickLin236/pomodoro-timer/releases/latest/download/Pomodoro.exe)

The download link becomes available after the first successful release workflow. Releases contain the standalone executable, an optional ZIP with install scripts/documentation, and SHA-256 checksums. Maintainer upload and release steps are in [GitHub releases](Docs/GitHub_Releases.md).

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

Extract the ZIP and run `Install.ps1` from PowerShell to copy the executable to `%LOCALAPPDATA%\Programs\PomodoroNative` and create a Start Menu shortcut. It requires no elevation. Sign-in startup remains off until enabled in the app. `Uninstall.ps1` removes the installed executable, its shortcut, and its own startup entry; timer data is preserved unless `-RemoveData` is supplied. Keep the extracted package for uninstall and measurement scripts.

You can also run the executable directly. Move it to its permanent location before enabling sign-in startup. `Pomodoro.exe --background` starts hidden or minimized according to the saved background mode.

## Use

- **Pomodoro:** set focus/break minutes and cycles, then Start. One cycle includes focus and break, including the final break. Pause/Resume, Skip phase, and Reset are available. Save setup during a session to configure the next session; the active setup remains displayed.
- **Timer:** enter hours, minutes, and seconds. It runs independently of Pomodoro.
- **Settings:** choose sound, sign-in startup, background display mode, and preferred taskbar source. Save settings applies changes. Test notification requests a local Windows banner.
- Closing or minimizing hides the window to the tray by default. Select **Keep taskbar status** to retain a minimized taskbar button. Open from the tray, or launch another copy to activate the existing instance. **Exit** saves and terminates the process.

State and settings are stored separately as UTF-8 JSON in `%LOCALAPPDATA%\PomodoroNative`. Each file has a last-known-good `.bak`. Save/recovery errors remain visible in the window. Running timers advance while the app is exited; paused timers retain their remaining time. On return after missed boundaries, one summary replaces a sequence of old notifications.

Windows controls notification delivery and tray placement. No notification can run while the process is exited or the PC is asleep/off. The next launch/resume reconciles time. A clock change while the process is exited affects UTC recovery. See [behavior](Docs/Behavior.md) for limits and exact semantics.

## Portable tests

The engine, JSON store, and controller build on Linux without the Windows UI:

```sh
cmake -S . -B build/portable -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/portable --parallel
ctest --test-dir build/portable --output-on-failure
```

With CMake 3.21+, `cmake --preset portable`, `cmake --build --preset portable`, and `ctest --preset portable` are equivalent. CMake 3.16+ supports the explicit commands (older CTest can use `cd build/portable` followed by `ctest --output-on-failure`). All dependencies are in this repository; no FetchContent, package restore, browser runtime, or service is required.

The three portable suites include 34 scenario groups and 25,000 deterministic randomized engine command steps. Windows builds add a `windows_ui` regression test that drives real native settings commands against an isolated registry key; it requires a Windows desktop and leaves the real startup entry untouched. Each CTest case has a 30-second timeout. The [validation record](Docs/Validation.md) lists the bugs reproduced and fixed during the audit, sanitizer results, and remaining Windows checks.

## Verification status

Portable Release tests and Windows cross-build results are recorded in [validation](Docs/Validation.md). Real Windows 10 shell behavior, MSVC builds, DPI interaction, PowerShell installation, and resource usage require the [Windows checklist](Docs/Windows_Test_Checklist.md). The provisional minimized private-byte target is 40 MiB; it is **not a measured result**. `Measure-Resources.ps1` records performance data rather than assuming that target has been met.

## Source layout

`Core` contains deterministic deadline arithmetic; `Persistence` owns validated JSON and durable file replacement; `App/Controller` handles clock anchoring, persistence, and notification attempts; `Platform` owns Windows shell/clock/startup APIs; `UI` owns native controls and the single message-loop scheduler. Tests inject `FakeClock`; real clock reads do not enter the engine.

The implementation follows [the development plan](Pomodoro_Development_Plan.md). Windows API references include [notification-area behavior](https://learn.microsoft.com/en-us/windows/win32/shell/notification-area), [QueryInterruptTime](https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-queryinterrupttime), and [CMake static CRT selection](https://cmake.org/cmake/help/latest/prop_tgt/MSVC_RUNTIME_LIBRARY.html).
