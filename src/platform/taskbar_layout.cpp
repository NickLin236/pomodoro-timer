#include "pomodoro/taskbar_layout.h"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace pomodoro {
namespace {
using Coordinate = std::int64_t;

bool clippedToBand(TaskbarRect obstacle, TaskbarRect strip,
                   Coordinate top, Coordinate bottom,
                   Coordinate& left, Coordinate& right) {
    left = (std::max)(strip.left, obstacle.left);
    right = (std::min)(strip.right, obstacle.right);
    const Coordinate obstacleTop = (std::max)(strip.top, obstacle.top);
    const Coordinate obstacleBottom = (std::min)(strip.bottom, obstacle.bottom);
    return left < right && obstacleTop < obstacleBottom &&
        obstacleTop < bottom && obstacleBottom > top;
}
} // namespace

bool taskbarSlotIsFree(TaskbarRect slot, TaskbarRect strip, int gap,
                      const std::vector<TaskbarRect>& obstacles) {
    if (strip.left >= strip.right || strip.top >= strip.bottom ||
        slot.left >= slot.right || slot.top >= slot.bottom || gap < 0 ||
        slot.left < strip.left || slot.right > strip.right ||
        slot.top < strip.top || slot.bottom > strip.bottom) return false;
    for (const auto& obstacle : obstacles) {
        Coordinate left, right;
        if (!clippedToBand(obstacle, strip, slot.top, slot.bottom, left, right)) continue;
        if (Coordinate{slot.right} + gap > left && Coordinate{slot.left} < right + gap)
            return false;
    }
    return true;
}

std::optional<TaskbarRect> findTaskbarSlot(
    TaskbarRect strip, int preferredRight, int width, int height, int gap,
    const std::vector<TaskbarRect>& obstacles) {
    const Coordinate stripWidth = Coordinate{strip.right} - strip.left;
    const Coordinate stripHeight = Coordinate{strip.bottom} - strip.top;
    if (stripWidth <= 0 || stripHeight <= 0 || width <= 0 || height <= 0 ||
        gap < 0 || width > stripWidth || height > stripHeight) return std::nullopt;

    const Coordinate top = Coordinate{strip.top} + (stripHeight - height) / 2;
    const Coordinate bottom = top + height;
    Coordinate right = (std::min)(Coordinate{preferredRight}, Coordinate{strip.right});
    if (right - width < strip.left) return std::nullopt;

    // Project only the visible part of each occupied window onto the panel's
    // row. A taller child clipped by Explorer still occupies taskbar pixels.
    std::vector<std::pair<Coordinate, Coordinate>> occupied;
    occupied.reserve(obstacles.size());
    for (const auto& obstacle : obstacles) {
        Coordinate left, rightEdge;
        if (clippedToBand(obstacle, strip, top, bottom, left, rightEdge))
            occupied.emplace_back(left - gap, rightEdge + gap);
    }

    std::sort(occupied.begin(), occupied.end());
    for (auto interval = occupied.rbegin(); interval != occupied.rend(); ++interval) {
        if (right > interval->first && right - width < interval->second) {
            // Moving left cannot make an intersecting panel fit to the right
            // of this interval. Its left edge is the next rightmost candidate.
            right = interval->first;
            if (right - width < strip.left) return std::nullopt;
        }
    }

    // All intermediates use int64; containment in the original int-coordinate
    // strip establishes that these final casts are representable.
    return TaskbarRect{static_cast<int>(right - width), static_cast<int>(top),
        static_cast<int>(right), static_cast<int>(bottom)};
}

} // namespace pomodoro
