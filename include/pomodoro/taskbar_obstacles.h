#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "pomodoro/taskbar_layout.h"

namespace pomodoro {

inline constexpr UINT taskbarObstaclesReadyMessage = WM_APP + 91;

struct TaskbarObstacleSnapshot {
    std::vector<TaskbarRect> occupied;
    std::optional<int> widgetLeft;
    // False while application-button geometry is unavailable or stale. The
    // caller must not display a panel based on incomplete occupied geometry.
    bool taskButtonsKnown = true;
};

// Call from a thread using the taskbar host's DPI context. Rectangles are in
// host-client coordinates. No existing taskbar window is moved or resized.
// Accessibility queries run on one capped background worker; completion posts
// taskbarObstaclesReadyMessage to panel with no pointer payload.
// Isolated synthetic-host tests pass false for includeTopLevel so unrelated
// desktop windows cannot become obstacles in their private test strip.
TaskbarObstacleSnapshot collectTaskbarObstacles(HWND host, HWND panel, bool includeTopLevel = true);

} // namespace pomodoro
#endif
