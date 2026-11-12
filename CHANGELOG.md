# Changelog

## 1.2.1 — November 11, 2026

- Attempt notification delivery when Windows reports `0x80070490` (Element not found) while reading notification settings. Previously, this lookup failure stopped both test and completion notifications before Windows received a display request. Explicit disabled settings and other lookup errors still report their existing failures.
- Place taskbar status to the left of the visible Traffic Monitor/weather/toolbar/notification-area widget group instead of drawing over the weather area. Detect Traffic Monitor even when its cross-process cached caption is unavailable, without resizing neighboring windows.
- Add regressions for the missing-settings notification path and taskbar widget placement. Windows desktop validation remains separate from portable tests and compilation.

## 1.2.0 — November 11, 2026

- Show either active Pomodoro or countdown status in the taskbar, using the preferred source when both timers are active and automatically falling back to the active timer.
- Find Traffic Monitor and the notification area through descendant windows instead of requiring one exact Explorer taskbar hierarchy. Horizontal taskbar layouts with a usable anchor can display the panel; Windows 11 placement remains unverified.
- Replace notification-area balloons with native Windows notifications containing timer-specific text, a **Dismiss** button, and a **Stop** button. Dismiss removes the notification; Stop resets only the session that created it, so an old notification cannot stop a newer session.
- Register notification identity and protocol activation per user, including a notification shortcut, when needed. The executable remains standalone and requires no administrator access or additional download.
- Report notification setup, policy, and delivery errors with details instead of a generic rejection message, and retry failed delivery without accumulating old notifications.
- Capture asynchronous Windows delivery failures, with bounded callback ownership and cleanup on reset, replacement, and exit.
- Add regression coverage for resetting during the final break, completing a countdown, starting another Pomodoro, reopening, and resetting at a phase boundary. These tests preserve every expected focus/break completion and full break duration; they do not establish the cause of the reported skipped break on the user's desktop.
- Add tests for notification content, actions, session isolation, and more varied taskbar nesting.

The taskbar panel remains an overlay on a primary horizontal taskbar and can cover application buttons when space is crowded. Vertical layouts remain unsupported. Real Windows notification delivery, Dismiss/Stop behavior, DPI/auto-hide, and Traffic Monitor coexistence require the Windows desktop checklist; portable tests and cross-compilation cannot validate those behaviors.

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
