# Validation record

Implementation date: November 9, 2026.

The development environment is Linux with GCC 9.4 and CMake 3.16.3. This environment cannot demonstrate Windows 10 desktop integration or measure the target application's runtime resource usage.

Completed checks:

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

`windows_ui_tests` drives the actual native Save Settings command with HKCU redirected to an isolated temporary key. It covers stale paths, enabling/disabling, external registry removal/replacement, quoting, background launch, and preference persistence. It creates no tray icon and does not access the real Run key. The test executable cross-compiles; execution remains pending on a Windows desktop.

Artifacts are under `build/windows-cross/Pomodoro.exe` and `build/windows-cross/Pomodoro-1.0.0-windows-x64.zip`. The cross-compiled binary is suitable for Windows validation; the documented MSVC Release build remains the intended release toolchain.

The manual Windows checklist remains pending. MSVC compilation, Windows 10 shell/UI interaction, PowerShell execution, sign-in registration, physical suspend/resume, resource measurements, and a fresh offline machine were **not tested**. No measured claim about the 40 MiB target is made. A portable test or cross-compilation does not establish those acceptance criteria.
