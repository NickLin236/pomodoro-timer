#include "pomodoro/clock.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstring>

namespace pomodoro {

ClockSample WindowsClock::now() const {
    // Resolve the Windows 10 API dynamically so older cross-compilation SDK
    // headers and import libraries can still build the native application.
    using QueryInterruptTimeFunction = void (WINAPI*)(PULONGLONG);
    static const auto queryInterruptTime = []() -> QueryInterruptTimeFunction {
        const auto base = GetModuleHandleW(L"kernelbase.dll");
        auto address = base ? GetProcAddress(base, "QueryInterruptTime") : nullptr;
        if (!address) {
            const auto kernel = GetModuleHandleW(L"kernel32.dll");
            address = kernel ? GetProcAddress(kernel, "QueryInterruptTime") : nullptr;
        }
        QueryInterruptTimeFunction function = nullptr;
        static_assert(sizeof(function) == sizeof(address));
        std::memcpy(&function, &address, sizeof(function));
        return function;
    }();
    FILETIME systemTime{};
    GetSystemTimeAsFileTime(&systemTime);
    ULARGE_INTEGER ticks{};
    ticks.LowPart = systemTime.dwLowDateTime;
    ticks.HighPart = systemTime.dwHighDateTime;
    ULONGLONG interruptTime = 0;
    // Unlike QueryUnbiasedInterruptTime, this count includes suspended time.
    // It is unaffected by manual/NTP adjustments to the wall clock.
    Millis monotonicMs;
    if (queryInterruptTime) {
        queryInterruptTime(&interruptTime);
        monotonicMs = static_cast<Millis>(interruptTime / 10000ULL);
    } else {
        // Defensive fallback: boot-relative GetTickCount64 also includes sleep
        // and ignores wall-clock changes, at the system timer's resolution.
        monotonicMs = static_cast<Millis>(GetTickCount64());
    }
    constexpr ULONGLONG windowsToUnixEpoch100ns = 116444736000000000ULL;
    const auto unixTicks = ticks.QuadPart - windowsToUnixEpoch100ns;
    return {static_cast<Millis>(unixTicks / 10000ULL), monotonicMs};
}

} // namespace pomodoro
#endif
