// Standalone raw-parser audit: include implementation intentionally to exercise
// private decoding boundaries without production API changes. Link timer_engine.cpp only.
#include "../../src/persistence/state_store.cpp"
#include <iostream>
#include <random>
#include <array>

int main() {
    using namespace pomodoro;
    std::mt19937_64 random(0x5345435552495459ULL);
    std::vector<std::string> corpus;
    corpus.push_back(encodeState(AppState{}));
    corpus.push_back(encodePreferences(Preferences{}));
    TimerEngine engine;
    if (!engine.startPomodoro({1, 1, maxCycles}, 0, "session-1") ||
        !engine.startCountdown(1, 0, "session-2")) return 2;
    AppState running;
    running.pomodoro = engine.pomodoro;
    running.countdown = engine.countdown;
    corpus.push_back(encodeState(running));
    engine.pausePomodoro(0);
    engine.pauseCountdown(0);
    AppState paused;
    paused.pomodoro = engine.pomodoro;
    paused.countdown = engine.countdown;
    corpus.push_back(encodeState(paused));
    const std::array<Millis, 9> times = {0, 1, 2000, 1700000000000LL, maxDurationMs,
        std::numeric_limits<Millis>::min(), std::numeric_limits<Millis>::min()+1,
        std::numeric_limits<Millis>::max()-1, std::numeric_limits<Millis>::max()};
    std::size_t cases = 0, parsed = 0, states = 0, preferences = 0;
    auto test = [&](const std::string& input) {
        ++cases;
        try { (void)JsonParser(input).parse(); ++parsed; } catch (const std::exception&) {}
        try { (void)decodePreferences(input); ++preferences; } catch (const std::exception&) {}
        try {
            const auto state = decodeState(input);
            ++states;
            for (const auto now : times) {
                TimerEngine sample;
                sample.pomodoro = state.pomodoro;
                sample.countdown = state.countdown;
                (void)sample.pomodoroRemaining(now);
                (void)sample.countdownRemaining(now);
                (void)sample.advance(now);
                sample.pausePomodoro(now); sample.pauseCountdown(now);
                sample.resumePomodoro(now); sample.resumeCountdown(now);
                sample.skipPomodoro(now);
                (void)sample.shiftDeadlines(now);
            }
        } catch (const std::exception&) {}
    };
    for (const auto& seed : corpus) test(seed);
    const std::array<std::string, 14> numbers = {"-9223372036854775809", "-9223372036854775808", "-1", "0", "1", "1000", "604800000", "9223372036854775806", "9223372036854775807", "9223372036854775808", "18446744073709551615", "18446744073709551616", "1e999999", "0.0"};
    for (const auto& seed : corpus) {
        for (std::size_t i = 0; i < seed.size(); ++i) {
            if (seed[i] != ':' || i+1 == seed.size() || seed[i+1] < '0' || seed[i+1] > '9') continue;
            const auto start = i+1;
            auto end = start;
            while (end < seed.size() && seed[end] >= '0' && seed[end] <= '9') ++end;
            for (const auto& number : numbers) { auto changed = seed; changed.replace(start, end-start, number); test(changed); }
        }
    }
    for (std::size_t n = 0; n < 100000; ++n) {
        auto input = corpus[random() % corpus.size()];
        const auto edits = 1 + random() % 12;
        for (std::size_t e = 0; e < edits; ++e) {
            const auto pos = random() % (input.size()+1);
            switch (random() % 5) {
            case 0: input.insert(pos, 1, static_cast<char>(random())); break;
            case 1: if (pos < input.size()) input.erase(pos, 1+random()%16); break;
            case 2: if (pos < input.size()) input[pos] = static_cast<char>(random()); break;
            case 3: input.insert(pos, numbers[random()%numbers.size()]); break;
            case 4: if (pos < input.size()) input.resize(pos); break;
            }
        }
        test(input);
    }
    for (unsigned depth = 0; depth < 100; ++depth) {
        test(std::string(depth, '[') + "0" + std::string(depth, ']'));
        test(std::string("{\"x\":") + std::string(depth, '[') + "0" + std::string(depth, ']') + "}");
    }
    for (auto size : {65535,65536,65537,131072}) test(std::string(size, ' '));
    std::cout << "cases=" << cases << " parsed=" << parsed << " valid_states=" << states << " valid_settings=" << preferences << '\n';
}
