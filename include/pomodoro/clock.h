#pragma once

#include "pomodoro/timer_engine.h"

namespace pomodoro {

struct ClockSample {
    Millis utcMs = 0;
    Millis monotonicMs = 0;
};

class Clock {
public:
    virtual ~Clock() = default;
    [[nodiscard]] virtual ClockSample now() const = 0;
};

// Fake time advances immediately and independently of the test runner's clock.
class FakeClock final : public Clock {
public:
    ClockSample sample;

    explicit FakeClock(ClockSample initial = {}) : sample(initial) {}
    [[nodiscard]] ClockSample now() const override { return sample; }
    void advance(Millis elapsedMs) {
        sample.utcMs += elapsedMs;
        sample.monotonicMs += elapsedMs;
    }
    void adjustWallClock(Millis adjustmentMs) { sample.utcMs += adjustmentMs; }
};

#ifdef _WIN32
class WindowsClock final : public Clock {
public:
    [[nodiscard]] ClockSample now() const override;
};
#endif

} // namespace pomodoro
