// Standalone deterministic boundary fuzzer; see README.md for sanitizer commands.
#include "pomodoro/notifications.h"
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace pomodoro;
int main() {
    std::uint64_t random = 0x35ec17845a3bad19ULL;
    const auto next = [&]() {
        random ^= random << 13; random ^= random >> 7; random ^= random << 17;
        return random;
    };
    const std::wstring bases[] = {
        L"pomodoro-native://stop/pomodoro/00112233445566778899aabbccddeeff",
        L"pomodoro-native://open/countdown/1700000000000-2",
        L"pomodoro-native://stop/pomodoro/", L""
    };
    std::size_t accepted = 0;
    for (std::size_t i = 0; i < 1'000'000; ++i) {
        std::wstring uri = bases[next() % 4];
        const auto edits = 1 + next() % 12;
        for (std::size_t j = 0; j < edits; ++j) {
            const auto position = static_cast<std::size_t>(next() % (uri.size() + 1));
            const auto value = static_cast<wchar_t>(next() % 0x110000);
            switch (next() % 4) {
            case 0: uri.insert(position, 1, value); break;
            case 1: if (position < uri.size()) uri.erase(position, 1); break;
            case 2: if (position < uri.size()) uri[position] = value; break;
            default: uri.append(next() % 210, value); break;
            }
        }
        const auto action = parseNotificationAction(uri);
        if (!action) continue;
        ++accepted;
        if (action->sessionId.empty() || action->sessionId.size() > 96 ||
            !std::all_of(action->sessionId.begin(), action->sessionId.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                    (c >= 'A' && c <= 'Z') || c == '-';
            })) throw std::runtime_error("accepted unsafe session");
        const std::wstring canonical = std::wstring(L"pomodoro-native://") +
            (action->command == NotificationCommand::Open ? L"open/" : L"stop/") +
            (action->timer == TimerKind::Pomodoro ? L"pomodoro/" : L"countdown/") +
            std::wstring(action->sessionId.begin(), action->sessionId.end());
        if (canonical != uri) throw std::runtime_error("accepted noncanonical payload");
    }
    std::cout << "1000000 deterministic URI mutations passed; " << accepted << " canonical acceptances\n";
}
