# Windows 10 Pomodoro timer — development plan

Prepared November 9, 2026. This is a proposed specification and implementation roadmap; no application has been built or benchmarked yet.

## 1. Recommended approach

Use **C++20, the native Win32 API, MSVC, and CMake**. Build a small, Unicode, per-user desktop application for Windows 10 x64, with standard Windows controls and a single main process.

This choice follows the priority of minimizing runtime memory. Native controls and Windows shell APIs avoid bundling a browser engine or running a managed-language runtime. The memory benefit is an architectural expectation, not a benchmark. C++ requires more care with ownership and native APIs, so isolate those details and use RAII for handles, icons, menus, COM objects, and file resources.

If development simplicity becomes the higher priority, C# with WinForms is a reasonable alternative. Its NotifyIcon component directly supports tray applications [1]. Measure a small prototype on the target Windows 10 edition before committing to its memory budget and runtime version.

Recommended baseline:

| Concern | Choice |
| --- | --- |
| Application | C++20 with Win32/common controls |
| Build | CMake presets and MSVC Release x64 |
| UI | One main window with Pomodoro, Timer, and Settings views |
| Tray and notifications | Shell_NotifyIconW |
| Taskbar | Window title plus ITaskbarList3 progress |
| Storage | Small versioned UTF-8 JSON files in LocalAppData |
| Persistence dependency | A pinned, vendored JSON library, such as nlohmann/json, if needed |
| Runtime timing | Monotonic elapsed clock, exposed through an injectable clock interface |
| Restart timing | Persisted UTC deadlines |
| Tests | Native unit tests run by CTest, using a fake clock |
| Distribution | Offline Release package with embedded icons/sound; stable per-user install location |

Development tools and third-party source can be acquired during setup. The released application must start and operate without network access or dependency downloads. Keep all required dependency source in the repository or an offline dependency bundle.

## 2. Proposed version-one behavior

These defaults make ambiguous behavior explicit. They can be changed before implementation without changing the main architecture.

| Feature | Proposed behavior |
| --- | --- |
| Pomodoro settings | Editable focus duration, break duration, and integer cycle count; defaults 25 minutes, 5 minutes, and 4 cycles |
| Cycle definition | One focus followed by one break; include the final break, then finish |
| Controls | Start, Pause, Resume, Skip phase, and Reset |
| Transitions | Automatically start the next phase at the previous phase's scheduled end |
| Settings during a session | Edits apply to the next new session; show the active session's captured settings |
| Pause | Freeze the remaining duration across closing, reopening, and rebooting |
| Skip | Advance immediately, treating that phase as finished for sequence progression; no automatic completion alert for a manual skip |
| Reset | Return that timer to its configured starting state |
| General timer | User-entered hours/minutes/seconds, with Start, Pause, Resume, and Reset |
| Independence | Pomodoro and general timer may run together, sharing one scheduler |
| Completion alerts | Request a local Windows notification at every automatic focus, break, and general-timer completion; optional local sound |
| Close button | Send the application into the selected background mode |
| Tray menu | Open, pause/resume each active timer, reset, settings, and Exit |
| Actual Exit | Save state and terminate the process; running deadlines still advance logically |
| Startup | Opt-in “Start when I sign in,” launching into the background |
| Taskbar source | Choose Pomodoro or general timer when both are running |
| Late recovery | Reconcile all elapsed phases and show one current summary rather than replaying many old banners |

Validate positive durations, require at least one cycle, impose documented upper bounds, and use checked 64-bit duration arithmetic. Invalid or empty input must not alter a running timer. Display HH:MM:SS when a duration exceeds an hour. Keep internal precision finer than the display's whole seconds.

A separate long break, history charts, accounts, cloud sync, themes, global shortcuts, and scheduled wake-up can follow version one if wanted.

## 3. Windows integration and its visible behavior

### Tray and closing

Use a stable tray-icon identity with Shell_NotifyIconW. The process remains alive when the main window is hidden, so timers and notifications continue. Windows and the user control whether the icon appears directly on the taskbar or in the hidden-icons overflow; the application should not attempt to force that placement [2].

Two background display choices resolve the taskbar/tray interaction:

| Background mode | Main window | Tray icon | Running taskbar button |
| --- | --- | --- | --- |
| Tray only, default | Hidden | Present | Removed |
| Keep taskbar status | Normally minimized | Present | Retained |

The close button follows the selected background mode. “Exit” in the tray menu is the explicit way to terminate the process. Reopening a second copy should activate the existing instance instead of starting competing timers.

Handle Explorer restarting: restore the tray icon after TaskbarCreated, and reapply taskbar status after TaskbarButtonCreated [3]. Dispose resources when they are no longer needed.

### Taskbar status

Use ITaskbarList3::SetProgressValue and SetProgressState for completion progress. Progress is associated with a window's taskbar button, so a tray-only application cannot simultaneously show that indicator on a removed button [4].

Update the window title with text such as “Focus 09:42 — 2/4.” Exact title text is visible in taskbar labels when the user's taskbar configuration shows labels; Windows 10's default grouping hides them [5]. With labels hidden, use the progress indicator and the window's hover preview/title. Exact remaining time is also available in the application and tray tooltip.

Use a static phase/status overlay only if useful. Do not rely on tiny changing overlay digits to make MM:SS readable. Do not change Explorer settings or implement a shell extension just to add an always-visible text strip.

### Notifications

For this Windows 10 target, use Shell_NotifyIconW balloon notifications. Microsoft documents that these are displayed as Windows 10 banner notifications and retained in Notification Center [6]. This uses the local shell and does not require a server or push-notification service.

Request one alert per automatic transition in normal operation, including the final break, and one alert when the general timer expires. Keep a completion message in the application's own status display, because Windows notification settings can suppress a banner [2]. Offer a local sound option and a “Test notification” control.

When the process is fully exited, it cannot run code to notify at a deadline. Similarly, a sleeping or powered-off PC does not show a live notification at that instant. On launch/resume, reconcile time and present one missed-completion summary. Supporting alarms while fully exited would require a separate Windows scheduling design; that is outside this version.

### Sign-in startup

Use an opt-in entry under:

`HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run`

Register a quoted executable path with a background-launch argument. This is a per-user sign-in action, rather than a service running before login. Microsoft documents both the Run key's logon semantics and that Windows may delay execution [7].

Place the executable in a stable location, for example under LocalAppData/Programs, before enabling startup. Toggling the option off removes only this application's entry. An installer/uninstaller should preserve that behavior. Report errors instead of silently showing an enabled checkbox when registration failed.

## 4. Timer engine and state recovery

The engine must be independent of windows, registry operations, notifications, and real clock reads. UI commands pass time into the engine; the engine returns updated state and transition events. Inject a fake clock in tests so multi-hour scenarios take milliseconds.

Keep two small models: PomodoroSession and CountdownTimer. Both use an Idle/Running/Paused/Completed status. Pomodoro additionally tracks Focus/Break, current cycle, and the session's captured configuration.

### Deadline-based timing

Store the end of the running phase:

`remaining = max(0, phaseEndsAtUtc - currentUtc)`

For the requested example, a focus period ending at 10:10 still ends at 10:10 after a true exit at 10:00. Relaunching at 10:05 reconstructs five minutes remaining.

While the process is running, anchor remaining duration to a monotonic Windows clock. QueryInterruptTime is independent of adjustments to the system's wall clock [8]. Use a clock that counts suspended time for the selected “elapsed time continues” policy, and verify suspend/resume on Windows. Keep the persisted UTC deadline aligned when a wall-clock change is detected, preserving the monotonic remaining duration.

Use UTC for storage; time-zone or daylight-saving display changes must not shift the schedule. If the system clock is changed while the application is fully exited, recovery must use that changed clock. A completely offline application cannot independently establish trusted elapsed time across every shutdown and manual clock change.

A timer message only asks the engine to reevaluate. Never make the countdown depend on subtracting one second for each callback.

### Crossing a boundary while absent

When now is later than the saved phase deadline:

1. Finish the elapsed phase.
2. Advance to the next phase, if any.
3. Calculate its deadline from the previous scheduled deadline plus the next duration.
4. Repeat until the current phase is in the future or all cycles are complete.
5. Persist the reconciled state and produce one recovery summary.

Anchoring the next phase to the scheduled boundary prevents reopening from giving the user an unintended fresh break.

Example: with 2-minute focus and 1-minute break, cycle 1 starts at 10:00. The app is reopened at 10:04. The correct state is cycle 2 focus with 1 minute left. This example assumes at least two configured cycles.

For a paused timer, restore pausedRemaining directly and do not subtract the application's absence.

### Saved state

Use a versioned state document containing, for each timer:

- Session identifier and status.
- Captured focus/break/cycle settings or general-timer duration.
- Current phase and cycle position.
- Current phase's full duration.
- UTC deadline when running.
- Remaining duration when paused.
- Transition sequence and notification-attempt information.
- Timestamp of the last saved state.

Keep user preferences in a separate small settings document, including startup preference, background mode, sound preference, and taskbar source.

Save on Start, Pause, Resume, Skip, Reset, every automatic transition, settings changes, and Exit. Persist enough at Start to recover even if the process is later killed. A running deadline does not need to be rewritten every second.

Use a temporary file in the same directory, flush it, replace the previous file safely, and keep a last-known-good backup. Validate the schema and all values on load. If the main document is corrupt, try the backup; if both are invalid, start safely and display a recovery message.

Save/recovery failures should be visible. Do not silently promise persistence when the state file cannot be written.

Store notification event identities to avoid routine repeated completion alerts after relaunch. Exactly-once display cannot be guaranteed across a crash between disk persistence and the shell notification call; the Windows shell may also suppress delivery. Prefer a single current summary after recovery and document that narrow limitation.

## 5. Small, testable project structure

| Component | Responsibility |
| --- | --- |
| Core/TimerEngine | State transitions, cycle rules, countdown math, and returned completion events |
| Core/Clock | UTC/monotonic clock interfaces and fake clock |
| Persistence/StateStore | JSON schema, validation, safe writes, backups, and migration |
| Platform/WindowsShell | Tray icon, notifications, taskbar status, and Explorer recovery |
| Platform/StartupRegistration | Per-user startup registration |
| App/Controller | Commands, persistence orchestration, scheduler, and recovery |
| UI/MainWindow | Native controls and rendering of current state |
| Tests | Engine, serialization/recovery, and controller tests |
| Resources | Embedded icons, manifest, and optional sound |
| Docs | Behavior specification, manual test checklist, and performance results |

One message loop and one shared timer are sufficient for version one. Native API calls belong outside the pure engine.

## 6. Development milestones for Codex

Build one milestone at a time. Each milestone ends with a build, relevant automated checks, and a short report identifying Windows-only checks that were actually run.

| Milestone | Deliverable | Acceptance check |
| --- | --- | --- |
| 1. Foundation and feasibility | CMake/MSVC project, native window, tray/banner/taskbar experiments, initial memory measurement | Release build runs on the target Windows 10 PC; shell features are demonstrated locally |
| 2. Timer engine | Pomodoro and general countdown models, commands, injected clock | Tests prove cycles, pause/resume, reset/skip, delayed callbacks, and final completion |
| 3. Persistence and recovery | Versioned storage, safe writes, backup recovery, overdue reconciliation | True Exit with 10 minutes left, relaunch 5 minutes later: approximately 5 minutes left |
| 4. User interface | Settings, countdown views, cycle display, controls, both timers integrated | Configuration and independent timers work with keyboard navigation and different DPI settings |
| 5. Windows integration | Production tray menu, notification events, display modes, startup, single instance, Explorer recovery | Tray/taskbar modes behave as specified; sign-in starts in background; alerts fire for all automatic boundaries |
| 6. Offline release and resource checks | Self-contained offline package, documentation, measured performance | Fresh Windows 10 test environment with networking disabled passes the full checklist |

For the native single-executable route, consider linking the Release CRT statically with /MT and embedding assets. Microsoft documents /MT as selecting the static runtime [9]. Verify the final package on a machine without development tools; static linkage alone is not a substitute for that test.

Use real Windows 10 for shell behavior and performance measurements. Engine tests may be run elsewhere if portable, but another operating system cannot validate Windows tray placement, notifications, taskbar integration, or sign-in behavior.

## 7. Resource budget

Set a provisional goal of **at most 40 MiB private bytes while minimized**, then revise only using measurements from the target PC. This is a development target, not a measured prediction. Also record working set, average CPU over a defined interval, thread count, and GDI/USER handle counts.

Implementation rules:

- Run one application process and share the scheduler between both timers.
- Refresh whole-second displays at most once per second while running.
- Stop periodic countdown callbacks when all timers are idle, paused, or completed.
- Avoid continuous animations, busy loops, per-second disk writes, and background network activity.
- Reuse icons, fonts, and other resources; verify handle counts during repeated operations.
- Keep logs bounded if logging is included.
- Measure Release builds, separately from debugger overhead.

Record a baseline after launch and after five minutes minimized, plus a longer running test. Exercise repeated start/pause/reset/minimize/restore operations and look for growth that fails to settle. Track file size separately from RAM; a small executable does not prove low memory use.

## 8. Acceptance checklist

Automate the engine and recovery cases with a fake clock:

- Custom focus/break durations and cycle counts, including one cycle.
- Every normal focus and break boundary, including the final break.
- Paused time across relaunch.
- True exit with 10 minutes remaining; return after 5 minutes.
- Return after the current phase, several phases, or the entire schedule has elapsed.
- General timer expiration while Pomodoro is also active.
- Late callbacks without timer drift.
- Duplicate transition processing and ordinary notification deduplication.
- Reset/skip behavior and changing settings during an active session.
- Missing, corrupt, invalid, and old-version state files.
- Invalid inputs, upper bounds, and arithmetic overflow.

Manually verify on Windows 10:

- Minimize and close to tray; restore; explicit Exit; relaunch.
- Taskbar status with labels visible and labels hidden.
- Both background display modes.
- Notification and sound at focus, break, and general-timer completion.
- Behavior with Windows notifications disabled.
- Suspend/resume, hibernate, reboot, and time-zone changes.
- Wall-clock changes while running and the documented recovery behavior after an exit.
- Sign-in startup on/off without elevation.
- Second launch and Explorer restart.
- High-DPI layouts, keyboard access, and readable countdown text.
- Offline installation/startup/run with required assets bundled.
- Release-build memory, CPU, and handle behavior.

## 9. How to start with Codex

Save the behavioral requirements in the repository, then ask Codex to implement the next milestone against them. Put persistent project constraints in AGENTS.md: native Windows 10 target, C++20/Win32/CMake, offline runtime, pure timer engine, deadline-based persistence, small dependency set, RAII, and the distinction between background close and actual Exit.

Use this initial request:

> Read the project development plan and create a C++20 Win32 desktop project for Windows 10 x64 using CMake and MSVC. The application must run offline and minimize memory use. Begin with milestone 1: provide a buildable native window and minimal demonstrations of tray notifications and taskbar progress, with embedded resources and clear Windows build/run instructions. Keep timing logic separate from Windows UI code. Report what you built, the checks you actually ran, and the remaining Windows-only validation. Complete this milestone before adding the full application.

Then use focused requests for the remaining milestones, for example:

> Implement the timer engine from the plan with an injectable clock. Add meaningful tests for focus/break sequencing, custom cycle counts, pause/resume, skip/reset, delayed callbacks, and independent general countdown. Keep it independent of UI and real clock reads.

> Implement persistence and restart recovery. Save running deadlines and paused remaining durations. Reconcile overdue phases from their original deadlines. Add the 10-minutes-left / reopen-5-minutes-later test, multi-phase recovery, and corrupted-file recovery tests.

> Connect the native UI and Windows integration to the tested engine. Implement both background modes, local completion alerts, per-user sign-in startup, single-instance handling, and recovery after Explorer restarts. Preserve offline operation and record Release-build resource measurements.

After each milestone, review the behavior and build result before requesting the next. Require clear distinction between a unit test that passed and an integration scenario that still needs the real Windows desktop.

## Official references

[1] [Microsoft: NotifyIcon component](https://learn.microsoft.com/en-us/dotnet/desktop/winforms/controls/notifyicon-component-windows-forms)

[2] [Microsoft: Notifications and the notification area](https://learn.microsoft.com/en-us/windows/win32/shell/notification-area)

[3] [Microsoft: Taskbar extensions](https://learn.microsoft.com/en-us/windows/win32/shell/taskbar-extensions)

[4] [Microsoft: ITaskbarList3::SetProgressValue](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-itaskbarlist3-setprogressvalue)

[5] [Microsoft: Customize the taskbar in Windows](https://support.microsoft.com/en-us/windows/experience/personalization/customize-the-taskbar-in-windows)

[6] [Microsoft: Shell_NotifyIconW](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shell_notifyiconw)

[7] [Microsoft: Run and RunOnce registry keys](https://learn.microsoft.com/en-us/windows/win32/setupapi/run-and-runonce-registry-keys)

[8] [Microsoft: QueryInterruptTime](https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-queryinterrupttime)

[9] [Microsoft: C runtime and C++ standard library files](https://learn.microsoft.com/en-us/cpp/c-runtime-library/crt-library-features)

