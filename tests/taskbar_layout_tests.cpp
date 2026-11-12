#include "pomodoro/taskbar_layout.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace pomodoro;

namespace {

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string(__func__) + ":" + std::to_string(__LINE__) + ": " #condition); } while (false)

bool same(const TaskbarRect& a, const TaskbarRect& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

void screenshotDoesNotCoverTheMonitorBesideWeather() {
    const TaskbarRect strip{0, 0, 2949, 60};
    const std::vector<TaskbarRect> widgets{
        {2210, -8, 2570, 70}, // Taller Traffic Monitor child, clipped to taskbar.
        {2590, 0, 2810, 60},
        {2810, 0, 2949, 60}
    };
    const auto slot = findTaskbarSlot(strip, 2590, 180, 56, 4, widgets);
    CHECK(slot && same(*slot, {2026, 2, 2206, 58}));
    CHECK(slot->right - slot->left == 180);
    CHECK(slot->right + 4 <= widgets.front().left);
    CHECK(taskbarSlotIsFree(*slot, strip, 4, widgets));
    CHECK(!taskbarSlotIsFree({2410, 2, 2590, 58}, strip, 4, widgets));
}

void clippedAndOversizedChildrenStillOccupyTheStrip() {
    const TaskbarRect strip{0, 0, 1000, 40};
    auto slot = findTaskbarSlot(strip, 900, 150, 36, 4, {{700, -80, 880, 90}});
    CHECK(slot && slot->right == 696);
    slot = findTaskbarSlot(strip, 1000, 150, 36, 4, {{960, -200, 1200, 200}});
    CHECK(slot && slot->right == 956);
    // Clip the actual child geometry instead of discarding it for exceeding
    // a guessed widget width or taskbar-height threshold.
    CHECK(!findTaskbarSlot(strip, 1000, 150, 36, 4, {{-200, -100, 1200, 100}}));
}

void overlappingAndReorderedWidgetsHaveOneSafeResult() {
    const TaskbarRect strip{0, 0, 1000, 40};
    std::vector<TaskbarRect> widgets{{600, 0, 800, 40}, {650, -20, 900, 55}, {850, 0, 950, 40}};
    const auto slot = findTaskbarSlot(strip, 1000, 180, 36, 6, widgets);
    CHECK(slot && slot->right == 594);
    std::reverse(widgets.begin(), widgets.end());
    const auto reordered = findTaskbarSlot(strip, 1000, 180, 36, 6, widgets);
    CHECK(reordered && same(*slot, *reordered));

    const auto gap = findTaskbarSlot(strip, 780, 80, 36, 4,
        {{400, 0, 500, 40}, {620, 0, 700, 40}});
    CHECK(gap && same(*gap, {536, 2, 616, 38}));
}

void touchingEdgesAndSeparateRowsRespectTheRequestedClearance() {
    const TaskbarRect strip{0, 0, 1000, 100};
    const std::vector<TaskbarRect> outsideBand{{0, 0, 1000, 40}, {0, 60, 1000, 100}};
    const auto unobstructed = findTaskbarSlot(strip, 900, 100, 20, 8, outsideBand);
    CHECK(unobstructed && same(*unobstructed, {800, 40, 900, 60}));
    CHECK(!findTaskbarSlot(strip, 900, 100, 20, 8, {{0, 39, 1000, 41}}));

    const std::vector<TaskbarRect> widget{{500, 0, 600, 100}};
    const auto touching = findTaskbarSlot(strip, 500, 100, 20, 0, widget);
    CHECK(touching && touching->right == 500);
    const auto exactGap = findTaskbarSlot(strip, 496, 100, 20, 4, widget);
    CHECK(exactGap && exactGap->right == 496);
    const auto onePixelShort = findTaskbarSlot(strip, 497, 100, 20, 4, widget);
    CHECK(onePixelShort && onePixelShort->right == 496);
    const auto rightOfWidget = findTaskbarSlot(strip, 704, 100, 20, 4, widget);
    CHECK(rightOfWidget && rightOfWidget->left == 604);
    CHECK(taskbarSlotIsFree({396, 10, 496, 30}, strip, 4, widget));
    CHECK(!taskbarSlotIsFree({397, 10, 497, 30}, strip, 4, widget));
    CHECK(taskbarSlotIsFree({604, 20, 704, 40}, strip, 4, widget));
    CHECK(!taskbarSlotIsFree({603, 20, 703, 40}, strip, 4, widget));
    CHECK(taskbarSlotIsFree({800, 40, 900, 60}, strip, 8, outsideBand));
}

void unavailableSpaceAndInvalidInputsDoNotProduceAnOverlap() {
    CHECK(!findTaskbarSlot({0, 0, 200, 40}, 200, 150, 36, 4, {{80, 0, 200, 40}}));
    CHECK(!findTaskbarSlot({20, 0, 10, 40}, 200, 10, 20, 4, {}));
    CHECK(!findTaskbarSlot({0, 20, 200, 20}, 200, 10, 20, 4, {}));
    CHECK(!findTaskbarSlot({0, 0, 200, 40}, 200, 0, 20, 4, {}));
    CHECK(!findTaskbarSlot({0, 0, 200, 40}, 200, 10, 0, 4, {}));
    CHECK(!findTaskbarSlot({0, 0, 200, 40}, 200, -1, 20, 4, {}));
    CHECK(!findTaskbarSlot({0, 0, 200, 40}, 200, 10, 20, -1, {}));
    CHECK(!findTaskbarSlot({0, 0, 200, 40}, 200, 201, 20, 4, {}));
    CHECK(!findTaskbarSlot({0, 0, 200, 40}, 200, 10, 41, 4, {}));
    CHECK(!findTaskbarSlot({10, 0, 200, 40}, 39, 30, 20, 4, {}));
    const auto emptyObstacles = findTaskbarSlot({0, 0, 200, 40}, 900, 100, 20, 4,
        {{0, 0, 0, 40}, {90, 0, 20, 40}, {100, 20, 150, 10}, {500, 0, 600, 40}});
    CHECK(emptyObstacles && same(*emptyObstacles, {100, 10, 200, 30}));
    const TaskbarRect strip{0, 0, 200, 40};
    CHECK(taskbarSlotIsFree({100, 0, 200, 10}, strip, 4, {}));
    CHECK(!taskbarSlotIsFree({-1, 0, 99, 10}, strip, 4, {}));
    CHECK(!taskbarSlotIsFree({101, 0, 201, 10}, strip, 4, {}));
    CHECK(!taskbarSlotIsFree({100, -1, 200, 9}, strip, 4, {}));
    CHECK(!taskbarSlotIsFree({100, 31, 200, 41}, strip, 4, {}));
    CHECK(!taskbarSlotIsFree({100, 0, 100, 10}, strip, 4, {}));
    CHECK(!taskbarSlotIsFree({100, 10, 200, 10}, strip, 4, {}));
    CHECK(!taskbarSlotIsFree({100, 0, 200, 10}, strip, -1, {}));
    CHECK(!taskbarSlotIsFree({100, 0, 200, 10}, {20, 0, 10, 40}, 4, {}));
    CHECK(!taskbarSlotIsFree({100, 0, 200, 10}, {0, 20, 200, 10}, 4, {}));
}

void negativeCoordinatesAndDpiScalingPreserveTheGeometry() {
    const auto negative = findTaskbarSlot({-3000, 100, 0, 160}, -200, 180, 56, 4,
        {{-700, 95, -250, 180}});
    CHECK(negative && same(*negative, {-884, 102, -704, 158}));

    const auto base = findTaskbarSlot({0, 0, 1000, 40}, 900, 120, 36, 4,
        {{700, -5, 900, 45}});
    const auto scaled = findTaskbarSlot({0, 0, 2000, 80}, 1800, 240, 72, 8,
        {{1400, -10, 1800, 90}});
    CHECK(base && scaled);
    CHECK(scaled->left == 2 * base->left && scaled->right == 2 * base->right);
    CHECK(scaled->top == 2 * base->top && scaled->bottom == 2 * base->bottom);
}

void extremeCoordinatesDoNotOverflow() {
    constexpr auto low = std::numeric_limits<int>::min();
    constexpr auto high = std::numeric_limits<int>::max();
    const auto large = findTaskbarSlot({low, low, high, high}, high, high, high, high, {});
    CHECK(large && same(*large, {0, -1073741824, high, 1073741823}));
    CHECK(!findTaskbarSlot({low, low, high, high}, high, high, high, high,
        {{low, low, high, high}}));
    const auto onePixel = findTaskbarSlot({low, 0, high, 20}, high, 1, 10, high,
        {{0, 0, 1, 20}});
    CHECK(onePixel && same(*onePixel, {low, 5, low + 1, 15}));
    CHECK(taskbarSlotIsFree(*onePixel, {low, 0, high, 20}, high, {{0, 0, 1, 20}}));
    CHECK(!taskbarSlotIsFree({low + 1, 5, low + 2, 15}, {low, 0, high, 20}, high,
        {{0, 0, 1, 20}}));
}

// Intentionally search every integer slot rather than using the solver's
// interval projections or jump strategy. This oracle checks rightmostness as
// well as collision freedom and full panel dimensions.
std::optional<TaskbarRect> bruteForceSlot(TaskbarRect strip, int preferredRight,
    int width, int height, int gap, const std::vector<TaskbarRect>& obstacles) {
    if (width <= 0 || height <= 0 || gap < 0 || strip.right - strip.left < width ||
        strip.bottom - strip.top < height) return std::nullopt;
    const int top = strip.top + (strip.bottom - strip.top - height) / 2;
    for (int right = (std::min)(strip.right, preferredRight); right - width >= strip.left; --right) {
        const TaskbarRect candidate{right - width, top, right, top + height};
        bool clear = true;
        for (const auto& obstacle : obstacles) {
            const int left = (std::max)(strip.left, obstacle.left);
            const int clippedRight = (std::min)(strip.right, obstacle.right);
            const int clippedTop = (std::max)(strip.top, obstacle.top);
            const int bottom = (std::min)(strip.bottom, obstacle.bottom);
            if (left >= clippedRight || clippedTop >= bottom ||
                candidate.bottom <= clippedTop || candidate.top >= bottom) continue;
            if (!(candidate.right + gap <= left || candidate.left >= clippedRight + gap)) {
                clear = false;
                break;
            }
        }
        if (clear) return candidate;
    }
    return std::nullopt;
}

void randomizedLayoutsMatchAnIndependentSlotSearch() {
    std::uint32_t state = 0xc4e71a95u;
    const auto integer = [&state](int low, int high) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return low + static_cast<int>(state % static_cast<unsigned>(high - low + 1));
    };
    for (int attempt = 0; attempt < 12000; ++attempt) {
        const int left = integer(-40, 10), top = integer(-5, 5);
        const TaskbarRect strip{left, top, left + integer(1, 100), top + integer(1, 30)};
        const int width = integer(1, strip.right - strip.left + 10);
        const int height = integer(1, strip.bottom - strip.top + 5);
        const int gap = integer(0, 5);
        const int preferred = integer(strip.left - 10, strip.right + 10);
        std::vector<TaskbarRect> obstacles;
        const int count = integer(0, 12);
        for (int i = 0; i < count; ++i) {
            const int x = integer(strip.left - 20, strip.right + 20);
            const int y = integer(strip.top - 15, strip.bottom + 15);
            obstacles.push_back({x, y, x + integer(-3, 50), y + integer(-3, 35)});
        }
        const auto expected = bruteForceSlot(strip, preferred, width, height, gap, obstacles);
        const auto actual = findTaskbarSlot(strip, preferred, width, height, gap, obstacles);
        CHECK(actual.has_value() == expected.has_value());
        if (actual) {
            CHECK(same(*actual, *expected));
            CHECK(taskbarSlotIsFree(*actual, strip, gap, obstacles));
        }
        std::reverse(obstacles.begin(), obstacles.end());
        const auto reordered = findTaskbarSlot(strip, preferred, width, height, gap, obstacles);
        CHECK(actual.has_value() == reordered.has_value());
        if (actual) CHECK(same(*actual, *reordered));
    }
}

} // namespace

int main() {
    try {
        screenshotDoesNotCoverTheMonitorBesideWeather();
        clippedAndOversizedChildrenStillOccupyTheStrip();
        overlappingAndReorderedWidgetsHaveOneSafeResult();
        touchingEdgesAndSeparateRowsRespectTheRequestedClearance();
        unavailableSpaceAndInvalidInputsDoNotProduceAnOverlap();
        negativeCoordinatesAndDpiScalingPreserveTheGeometry();
        extremeCoordinatesDoNotOverflow();
        randomizedLayoutsMatchAnIndependentSlotSearch();
        std::cout << "8 taskbar layout scenarios passed (12,000 randomized layouts)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
