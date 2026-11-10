# Windows 10 validation checklist

Record Windows edition/build, DPI/monitor layout, compiler/SDK version, executable hash, and Release configuration. Unchecked items below have not been validated by portable tests.

## Build and offline installation

- [ ] Configure/build/test with the documented MSVC x64 presets.
- [ ] Create ZIP, copy to a clean Windows 10 x64 machine with networking disabled, extract, run.
- [ ] Install per-user without elevation; launch using the Start Menu shortcut.
- [ ] Run from a non-ASCII user/profile path and a path containing spaces.
- [ ] Update after Exit; uninstall preserves data by default; optional data removal removes only app data.

## Timer behavior

- [ ] Default and custom focus/break/cycles, including one cycle and its final break.
- [ ] Pause/resume, skipped running/paused phase, final skip, reset, restart after completion.
- [ ] Both timers run together and alert independently. Invalid/empty/oversized inputs preserve active timers.
- [ ] Save setup during a session; captured setup stays active, next session/reset uses new setup.
- [ ] True Exit with ten minutes remaining, return five minutes later: five minutes remain.
- [ ] Pause, Exit, reboot, relaunch: paused time unchanged.
- [ ] Return after one/many/all phases: reconcile from original deadlines; only one summary.
- [ ] Kill process after Start; recover from the initially saved deadline.
- [ ] Corrupt primary state; verify backup warning/recovery. Corrupt both; verify safe defaults and warning.
- [ ] Deny write access to data directory; errors are visible, failed close/save does not silently promise persistence.

## Shell, clocks, accessibility

- [ ] Close/minimize in both background modes, restore, and actual Exit.
- [ ] Tray Open, settings, independent pause/resume and reset commands, Exit; keyboard context menu.
- [ ] Second launch opens existing window without competing timers.
- [ ] Restart Explorer; tray icon, menu, and taskbar progress return.
- [ ] Taskbar source switching, paused state, hidden/visible taskbar labels.
- [ ] Every automatic focus/break/countdown completion and notification test with sound on/off.
- [ ] Windows notifications disabled: in-app completion status remains available.
- [ ] Sign-in startup on/off without elevation; launching background mode works after sign-in.
- [ ] Suspend/resume, hibernate, reboot, time-zone/DST changes, manual forward/backward clock changes.
- [ ] Running clock adjustment preserves elapsed time; clock adjustment after Exit has documented UTC effect.
- [ ] Keyboard Tab/Shift+Tab, tab selection, Space/Enter for controls, readable text at 100/125/150/200% DPI.
- [ ] Different DPI monitors, small work areas, vertical scroll, and repeated restore/minimize.

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
