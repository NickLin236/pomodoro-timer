# Pomodoro Native

Offline Pomodoro and countdown timers for **Windows 10 x64**, built with C++20 and Win32. Includes notifications, tray controls, taskbar status, and restart recovery.

## Download

**[Download Pomodoro.exe](https://github.com/NickLin236/pomodoro-timer/releases/latest/download/Pomodoro.exe)** · [All releases](https://github.com/NickLin236/pomodoro-timer/releases)

Save the EXE in a permanent folder and run it. No installation, repository clone, or extra runtime download is required. **Exit the running copy before updating.**

The optional release ZIP includes `Install.ps1` and `Uninstall.ps1` for per-user installation.

## Use

- **Pomodoro:** choose focus/break durations and cycles. Each cycle includes a break, including the final cycle. Pause, Resume, Skip phase, and Reset are available.
- **Timer:** run an independent countdown, alongside Pomodoro if needed.
- **Minimize:** hide in the notification area while timers continue. **Close, Alt+F4, or Exit:** save and quit.
- **Notifications:** Dismiss clears the message; Stop resets only the session that produced it.
- **Settings:** choose sound, sign-in startup, taskbar status, and the preferred timer. Sign-in startup is opt-in.

Settings and timer state live in `%LOCALAPPDATA%\PomodoroNative`. Running timers recover elapsed time when reopened; notifications require the app to be running.

Taskbar status targets the primary horizontal taskbar and hides when no safe gap can be measured. If it is missing or overlaps another program, use **Settings → Copy taskbar diagnostics** and include the report in your bug report. Windows desktop verification remains incomplete; see the [validation record](Docs/Validation.md).

## Build and test

Requires **CMake 3.21+** and a C++20 compiler. Windows builds need Visual Studio 2022 with **Desktop development with C++** and a Windows 10 SDK.

Windows, from a developer PowerShell:

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-release
ctest --preset windows-release
```

Run `build/windows-msvc/Release/Pomodoro.exe`. To create the optional ZIP:

```powershell
cpack --config build/windows-msvc/CPackConfig.cmake -C Release
```

Portable tests on Linux:

```sh
cmake --preset portable
cmake --build --preset portable
ctest --preset portable
```

More details: [Behavior](Docs/Behavior.md) · [Windows checklist](Docs/Windows_Test_Checklist.md) · [Security review](Docs/Security_Review.md) · [Publishing releases](Docs/GitHub_Releases.md) · [Changelog](CHANGELOG.md).
