# Changelog

## 1.1.0 — November 10, 2026

- Close and Alt+F4 now save and exit the application, matching Exit.
- Minimize hides the main window in the notification area while timers continue.
- Added a Pomodoro text status panel for active focus/break sessions, with phase, remaining time, and cycle position. Paused sessions retain paused status; reset and final completion hide the panel.
- Added the default-on **Show Pomodoro status in taskbar** preference. Existing settings remain compatible and default the new preference on.
- Removed the obsolete background-mode selection from Settings. Legacy saved values remain readable but no longer alter Close or Minimize.
- Extended persistence and native UI regressions for preference compatibility and saving/terminating on Close.

The status panel targets the primary horizontal Windows 10 taskbar, immediately left of Traffic Monitor when detected or left of the notification area otherwise. It is an overlay and does not reserve taskbar space or resize Traffic Monitor; a crowded application-button area may require leaving a gap or disabling the panel. Vertical taskbars and the modern Windows 11 taskbar are unsupported. Exact placement, taskbar auto-hide, DPI behavior, and coexistence with Traffic Monitor require Windows desktop validation. The release remains a standalone executable; no extra application file is required to download and run it.

## 1.0.0

- Initial native Windows Pomodoro and independent countdown timers.
- Pause/resume, phase skipping, captured session settings, local notifications, per-user sign-in startup, and restart recovery.
- Versioned UTF-8 JSON storage with atomic writes and backups.
- Portable fake-clock tests, Windows UI regression coverage, and automated GitHub executable releases.
