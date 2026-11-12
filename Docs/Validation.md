# Validation record

Initial implementation: November 9, 2026. Version 1.1.0 update: November 10, 2026. Version 1.2.0 update: November 11, 2026.

The development environment is Linux with GCC 9.4 and CMake 3.16.3. This environment cannot demonstrate Windows 10 desktop integration or measure the target application's runtime resource usage.

Version 1.2.0 replaces notification-area balloons with actionable native Windows notifications, separates Pomodoro/countdown content, adds countdown taskbar status, and broadens horizontal taskbar anchor discovery. Notification identity, shortcut, and protocol activation are registered per user; the executable remains standalone. Stop actions carry their timer/session identity so an old notification cannot stop a new session. Failed synchronous requests have at most two five-second retries, canceled when that session resets or changes; no persistent notification outbox is claimed.

The reported final-break Reset → countdown → new Pomodoro sequence is now covered by deterministic controller regressions, including reopening and reset exactly at a phase boundary. These tests pass with the existing engine/controller, bringing controller coverage to 15 scenarios. The tests confirm fresh completion sequences and full expected break durations; they do not establish why a break appeared skipped or notifications failed on the user's Windows desktop. Late polling still deliberately catches up elapsed phases against their original deadlines.

Current 1.2.0 checks distinguish portable logic and compilation from Windows runtime behavior. All four portable Release CTest suites pass (0.71 seconds), including 8 notification scenarios and 15 controller scenarios. Notification checks cover timer-specific content, action parsing, session isolation, and bounded retries. Native UI coverage also includes `WM_COPYDATA` Stop routing, stale/test/malformed actions, and Stop at a due boundary without emitting a pending completion notice. The complete Windows application and all five test executables cross-compile successfully. The new native backend also passes warning-as-error compilation. Native coverage includes creating normal/silent notification XML, toast objects, and delivery-failure subscriptions without registration or sending notifications, plus malformed XML rejection. These Windows tests have been compiled, not executed in this Linux environment. The backend ABI was independently checked against Microsoft's SDK definitions; live delivery and callback execution still require Windows validation. The Windows checklist includes Dismiss/history removal, Stop for each timer, disabled notifications and recovery, repeated delivery, both taskbar timer sources, and the user's reset sequence. These desktop checks remain pending. Windows 11 anchor discovery is best effort and unverified; vertical taskbars remain unsupported. The panel remains an overlay and may cover crowded application buttons.

Version 1.2.0 uses matching CMake, resource, and manifest versions, a new changelog section, and a new `v1.2.0` release tag. A local package or version bump does not publish the GitHub release; publication occurs only after the tagged workflow succeeds. The earlier user-reported CI result and the 1.1.0 checks below do not establish the new notification backend's Windows runtime behavior.

Notification integration follows Microsoft's [protocol activation with a stub shortcut CLSID](https://learn.microsoft.com/en-gb/answers/questions/1016/push-notification-in-uwp-after-covert) guidance. The Dismiss action uses the [system dismiss behavior](https://learn.microsoft.com/en-us/dotnet/api/microsoft.toolkit.uwp.notifications.toastbutton.setdismissactivation), which dismisses the notification without activating the application. These API references support the implementation approach; they are not a Windows runtime test result for this application.

Version 1.1.0 changes Close/Alt+F4 to save and exit, Minimize to hide in the notification area, and adds an optional Pomodoro status overlay on the primary horizontal Windows 10 taskbar. New regression coverage checks migration of `showTaskbarStatus` and saving/destroying the window on Close. The panel's real positioning beside Traffic Monitor, crowded-taskbar behavior, auto-hide, DPI changes, and Explorer restart still require Windows desktop validation. The overlay does not reserve space and can cover a crowded application-button area.

The user reported that the previous GitHub CI run passed its MSVC build and all four Windows suites after the macro and test-manifest fixes. That reported result applies to the earlier source, not automatically to the new 1.1.0 panel implementation.

Version 1.1.0 checks completed in this session:

- All three portable Release CTest suites pass, including the new legacy-settings/default-on/off/type-validation cases for taskbar status.
- The complete Windows x64 executable and native UI test executable cross-compile successfully. The new panel and synthetic-host tests also pass warning-as-error compilation.
- Native UI coverage now includes synthetic taskbar/Traffic Monitor placement, unchanged neighboring window geometry, phase/countdown text, click forwarding, hide/show, recreation, cleanup, and real Close saving/destruction/`WM_QUIT`. These native tests are compiled here; execution is pending Windows CI.
- CMake, embedded resource, and manifest versions agree on 1.1.0. Workflow YAML, publish Bash, and source-matched changelog-note generation have been checked. The release workflow now takes version-specific notes from the build's hashed metadata.
- The executable still imports only Windows system DLLs and requires no companion application file.
- Engine and controller ASan/UBSan runs passed. The persistence sanitizer binary passed direct retries but also produced intermittent recursive `DEADLYSIGNAL` output in this container; those runs were interrupted. An independent non-PIE ASan/UBSan build passed, and the current Release persistence suite passed Valgrind with zero errors and zero outstanding bytes. The intermittent sanitizer cause is not proven; it is retained as a verification limitation.

No Windows 1.1.0 runtime result is claimed here.

Completed checks from the initial implementation and bug audit:

- Portable CMake Release build completed with GCC 9.4, `-Wall -Wextra -Wpedantic`.
- CTest passed all 3 portable suites (12 engine scenarios, 9 persistence groups, 13 controller scenarios). Tests use explicit checks that remain active in Release builds. Engine coverage includes 25,000 deterministic randomized command steps checked against an independent elapsed-time model.
- Fake-clock cases cover final breaks, independent timers, precise pause/resume, delayed callbacks, UTC restart, rebooted monotonic epoch, overdue schedules, skipped phases, session-captured settings, notification attempts, wall-clock jumps, and stopping per-second state writes.
- Persistence tests include missing, corrupt, invalid, oversized, malformed UTF-8, unknown-version and arithmetic-overflow documents, atomic backup recovery, preservation of good backups, and failed writes.
- All 3 sanitizer binaries passed individually with AddressSanitizer and UndefinedBehaviorSanitizer, with leak detection disabled for this container. One sanitizer invocation through CTest stalled and was interrupted; the same binaries then passed when launched directly. CTest cases now have 30-second timeouts.
- Valgrind Memcheck passed all 3 portable binaries: zero detected errors, zero bytes/blocks outstanding at exit. This checks portable code; it does not measure Windows resource usage or Windows handle lifetimes.
- The complete Windows x64 application, including native UI, shell integration, Windows file operations, icon/version/manifest resources, compiled and linked using temporary MinGW GCC 9.3 / MinGW-w64 7.0 tooling. Compiler dependencies were downloaded only for development and verified against Ubuntu package metadata. No compiler or external dependency is part of the runtime.
- Manifest XML and ICO structure validated. Packaging output is an offline ZIP containing the executable, scripts, and documentation.
- The PE import table contains only Windows system DLLs (no MinGW, C++ runtime, or other bundled third-party DLL is required). The x64 GUI executable is approximately 3.4 MiB; file size is not a memory measurement.

The follow-up bug audit reproduced and fixed:

- Backward UTC changes while fully exited capped displayed remaining time at the nominal duration and lost restored time on Pause. Remaining time now follows the saved UTC deadline, including pause/persist/resume when it exceeds the nominal duration.
- Stale temporary-file names caused save failures and deleted pre-existing files that the save did not own. Exclusive creation now retries collisions, and cleanup only removes owned files.
- Global numeric locale could introduce digit grouping into saved JSON. Serialization now uses the classic locale.
- A data-directory path that was actually a regular file silently loaded defaults. Loading now reports the invalid storage path.
- Saving startup settings relied on a cached checkbox state and could leave an old executable registered, or fail to repair an externally removed registration. Explicit Save now reconciles the actual per-user registry value.
- A failed scheduler registration set its error message after the last status paint. The error is now displayed immediately with the window accessible.

`windows_ui_tests` drives the actual native Save Settings command with HKCU redirected to an isolated temporary key. Its earlier coverage includes stale paths, enabling/disabling, external registry removal/replacement, quoting, background launch, and preference persistence. Version 1.1.0 extends it with the taskbar-status preference and saving/terminating on Close. It creates no tray icon and does not access the real Run key. The new test version still needs execution on Windows.

Version 1.1.0 build outputs use `build/windows-cross/Pomodoro.exe` and `Pomodoro-1.1.0-windows-x64.zip`; distribution copies may also be placed in `dist/`. A cross-compiled binary is suitable for Windows validation; the documented MSVC Release build remains the intended release toolchain. The GitHub release is updated only after its matching tag workflow succeeds.

The new version's manual Windows checklist remains pending. This Linux session has not executed the Windows 1.1.0 status panel, PowerShell installation, sign-in registration, physical suspend/resume, resource measurements, or a fresh offline machine. No measured claim about the 40 MiB target is made. A portable test or cross-compilation does not establish those acceptance criteria.
