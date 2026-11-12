# Windows 10 validation checklist

Record Windows edition/build, DPI/monitor layout, compiler/SDK version, executable hash, and Release configuration. Unchecked items below have not been validated by portable tests.

## Build and offline installation

- [ ] Configure/build/test with the documented MSVC x64 presets.
- [ ] Create ZIP, copy to a clean Windows 10 x64 machine with networking disabled, extract, run.
- [ ] Install per-user without elevation; launch using the Start Menu shortcut.
- [ ] Run from a non-ASCII user/profile path and a path containing spaces.
- [ ] Update after Exit; uninstall preserves data by default; optional data removal removes only app data.
- [ ] After notification registration, uninstall removes the installed copy's notification shortcut and protocol. Registration pointing to another portable copy remains intact.

## Timer behavior

- [ ] Default and custom focus/break/cycles, including one cycle and its final break.
- [ ] Pause/resume, skipped running/paused phase, final skip, reset, restart after completion.
- [ ] Both timers run together and alert independently. Invalid/empty/oversized inputs preserve active timers.
- [ ] Set 1-minute focus, 1-minute break, and 2 cycles. During the final break click Reset, run a short countdown to completion, then start another Pomodoro. Both focus phases and both full breaks run in order, with notifications at each boundary.
- [ ] Repeat Reset just as a phase ends; restart the app between sessions and confirm no old deadline, phase, cycle, or notification sequence leaks into the new session.
- [ ] Save setup during a session; captured setup stays active, next session/reset uses new setup.
- [ ] True Exit with ten minutes remaining, return five minutes later: five minutes remain.
- [ ] Pause, Exit, reboot, relaunch: paused time unchanged.
- [ ] Return after one/many/all phases: reconcile from original deadlines; only one current summary per affected timer.
- [ ] Kill process after Start; recover from the initially saved deadline.
- [ ] Corrupt primary state; verify backup warning/recovery. Corrupt both; verify safe defaults and warning.
- [ ] Deny write access to data directory; errors are visible, failed close/save does not silently promise persistence.

## Shell, clocks, accessibility

- [ ] Close using X and Alt+F4: state saves, the process exits, and its notification icon/status panel disappear.
- [ ] Minimize: main window and taskbar button hide, notification icon remains, timers continue; restore from the icon.
- [ ] With tray creation unavailable, minimizing retains an accessible normal taskbar button.
- [ ] Tray Open, settings, independent pause/resume and reset commands, Exit; keyboard context menu.
- [ ] Second launch opens existing window without competing timers.
- [ ] Restart Explorer during focus/break; notification icon, menu, and taskbar status return without duplicate panels.
- [ ] Start a Pomodoro: phase, remaining time, and cycle appear as text in the taskbar area.
- [ ] Start only a countdown: its remaining time appears in the taskbar area, including after Minimize.
- [ ] Run both timers; switch the preferred taskbar timer, then reset/complete that timer. The panel falls back to the other active timer.
- [ ] Pause/resume, focus/break transition, reset, and final break completion update/hide the text panel correctly. Paused status remains visible; the panel hides when neither timer is active.
- [ ] Turn Show timer status in taskbar off/on, save, relaunch; preference is retained. Existing 1.0 settings default it on.
- [ ] Traffic Monitor running, stopped, moved/resized, or restarted: the Pomodoro panel remains beside it and does not cover its readings.
- [ ] Test with a crowded taskbar: confirm the documented overlay limitation, leave sufficient space or disable the panel, and ensure Traffic Monitor readings remain uncovered.
- [ ] Taskbar auto-hide, resizing, screen resolution change, and 100/125/150/200% DPI: status follows the supported taskbar location.
- [ ] Vertical/unsupported taskbars fail gracefully with the documented fallback; no invisible window traps access to the app.
- [ ] On Windows 11, record whether descendant anchor discovery finds a usable horizontal layout and whether the panel is visible above sibling windows. Do not treat a successful build as placement validation.
- [ ] Main-window indicator source switching, paused state, and hidden/visible taskbar labels.
- [ ] Sign-in startup on/off without elevation; launching background mode works after sign-in.
- [ ] Suspend/resume, hibernate, reboot, time-zone/DST changes, manual forward/backward clock changes.
- [ ] Running clock adjustment preserves elapsed time; clock adjustment after Exit has documented UTC effect.
- [ ] Keyboard Tab/Shift+Tab, tab selection, Space/Enter for controls, readable text at 100/125/150/200% DPI.
- [ ] Different DPI monitors, small work areas, vertical scroll, and repeated restore/minimize.

## Notifications and actions

- [ ] Run the standalone executable from its permanent folder without installation or elevation. Test notification succeeds; the per-user Pomodoro Notifications shortcut and protocol point to that executable.
- [ ] Every automatic focus/break/countdown completion and notification test with sound on/off. Repeat the full final-break Reset → countdown → new Pomodoro sequence above.
- [ ] Pomodoro notifications contain only Pomodoro state; countdown notifications contain only countdown state. Simultaneous completion produces a separate relevant message for each timer.
- [ ] Click **Dismiss** on a notification banner and on its entry in Windows notification history. The entry disappears and both timers retain their state.
- [ ] Click **Stop** during Pomodoro focus or break. That Pomodoro resets and its taskbar status updates; an independent countdown continues.
- [ ] Click **Stop** for a countdown while Pomodoro runs. Only the countdown resets.
- [ ] Leave an old notification, reset/start a new session, then click the old Stop action. The new session continues. Repeat after closing and reopening the app.
- [ ] Click the notification body; the existing instance opens the related timer page. After Exit, the notification action launches only one instance and resolves its target session correctly.
- [ ] New phase messages replace the same timer's old entry; Pomodoro and countdown entries do not replace each other.
- [ ] Disable Windows notifications for the app and test again. A useful policy/delivery error is shown and in-app completion status remains available. Re-enable notifications and retry successfully.
- [ ] Test Focus Assist/Do not disturb and Windows sound settings; record suppressed banners/sounds separately from rejected requests.
- [ ] Force a synchronous delivery failure, then restore delivery before the two five-second retries expire. No duplicate/stale notification is left behind; resetting/starting another session cancels its old retries.
- [ ] Move the executable, run it from the new permanent location, and verify shortcut/protocol registration and notification actions use the new path. Repeat with spaces and non-ASCII characters in that path.
- [ ] Restart Explorer between notifications and confirm both notification test and subsequent automatic completions still work.

## Resource measurements

The target is at most 40 MiB minimized private bytes. It remains provisional until measured on Windows 10.

```powershell
.\Measure-Resources.ps1 -Launch -Executable .\Pomodoro.exe -DurationSeconds 360 -IntervalSeconds 5 -CsvPath baseline.csv
```

- [ ] Record launch baseline and five minutes minimized; private bytes, working set, normalized CPU, threads, GDI/USER and total handles.
- [ ] Repeat with both timers running and minimized for at least 30 minutes.
- [ ] Repeat 100 start/pause/reset/minimize/restore operations and check memory/handles settle.
- [ ] Verify no periodic callbacks while idle/paused/completed and no per-second writes or network activity.
- [ ] Record executable/package size separately from memory and retain CSV results in the test report.
