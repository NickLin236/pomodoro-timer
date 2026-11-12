#pragma once

#include <optional>
#include <vector>

namespace pomodoro {

// Half-open coordinates, matching Win32 RECT without depending on Windows.
struct TaskbarRect {
    int left;
    int top;
    int right;
    int bottom;
};

// Find the rightmost full-size panel whose right edge is <= preferredRight.
// The panel is vertically centered within strip. Every obstacle is clipped to
// strip; obstacles overlapping the panel's vertical band require horizontal
// clearance of at least gap pixels. Empty/inverted obstacles are ignored.
// Invalid strip/dimensions, negative gap, and insufficient room return nullopt.
// No existing window is moved, and the panel is never shrunk to fit.
[[nodiscard]] std::optional<TaskbarRect> findTaskbarSlot(
    TaskbarRect strip, int preferredRight, int width, int height, int gap,
    const std::vector<TaskbarRect>& obstacles);

// Validate an actual native window rectangle after positioning. Unlike the
// solver, this accepts any vertical position contained by strip. Invalid or
// empty rectangles, negative gap, and occupied pixels/clearance return false.
[[nodiscard]] bool taskbarSlotIsFree(TaskbarRect slot, TaskbarRect strip, int gap,
    const std::vector<TaskbarRect>& obstacles);

} // namespace pomodoro
