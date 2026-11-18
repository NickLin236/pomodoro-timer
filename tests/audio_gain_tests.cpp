#include "pomodoro/audio_gain.h"

#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

using namespace pomodoro;

namespace {
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string(__func__) + ":" + std::to_string(__LINE__) + ": " #condition); } while (false)

using Bytes = std::vector<std::uint8_t>;
void put(Bytes& bytes, std::size_t at, std::uint64_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (i * 8));
}
void append(Bytes& bytes, std::int64_t value, unsigned count) {
    const auto at = bytes.size(); bytes.resize(at + count);
    put(bytes, at, static_cast<std::uint64_t>(value), count);
}
Bytes wave(unsigned bits, Bytes samples, unsigned codec = 1) {
    Bytes bytes{'R','I','F','F',0,0,0,0,'W','A','V','E',
        'f','m','t',' ',16,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,
        'd','a','t','a',0,0,0,0};
    put(bytes, 20, codec, 2); put(bytes, 24, 1000, 4);
    put(bytes, 28, 1000 * bits / 8, 4); put(bytes, 32, bits / 8, 2); put(bytes, 34, bits, 2);
    put(bytes, 40, samples.size(), 4);
    bytes.insert(bytes.end(), samples.begin(), samples.end());
    if (samples.size() & 1) bytes.push_back(0);
    put(bytes, 4, bytes.size() - 8, 4);
    return bytes;
}
void rejected(const Bytes& bytes, int gain = 200) {
    bool threw = false;
    try { (void)applyWaveGain(bytes, gain); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

void pcm16GainSaturatesAndPreservesTheDocument() {
    Bytes data;
    for (const auto sample : {0, 1000, -1000, 20000, -20000, 32767, -32768}) append(data, sample, 2);
    const auto source = wave(16, data);
    CHECK(applyWaveGain(source, 100) == source);
    Bytes expected;
    for (const auto sample : {0, 2000, -2000, 32767, -32768, 32767, -32768}) append(expected, sample, 2);
    CHECK(applyWaveGain(source, 200) == wave(16, expected));
    CHECK(source == wave(16, data));
    CHECK(applyWaveGain(source, 0) == wave(16, Bytes(data.size(), 0)));
    expected.clear();
    for (const auto sample : {0, 500, -500, 10000, -10000, 16383, -16384}) append(expected, sample, 2);
    CHECK(applyWaveGain(source, 50) == wave(16, expected));

    auto metadata = source;
    const Bytes chunk{'L','I','S','T',3,0,0,0,'a','b','c',0};
    metadata.insert(metadata.end(), chunk.begin(), chunk.end());
    put(metadata, 4, metadata.size() - 8, 4);
    const auto adjusted = applyWaveGain(metadata, 200);
    CHECK(adjusted.size() == metadata.size());
    CHECK(Bytes(adjusted.begin(), adjusted.begin() + 44) == Bytes(metadata.begin(), metadata.begin() + 44));
    CHECK(Bytes(adjusted.end() - chunk.size(), adjusted.end()) == chunk);
}

void otherPcmWidthsUseCorrectSignednessAndLimits() {
    CHECK(applyWaveGain(wave(8, {0, 64, 128, 192, 255}), 200) == wave(8, {0, 0, 128, 255, 255}));
    CHECK(applyWaveGain(wave(8, {0, 64, 128, 192, 255}), 50) == wave(8, {64, 96, 128, 160, 191}));
    CHECK(applyWaveGain(wave(8, {0, 64, 128, 192, 255}), 0) == wave(8, {128, 128, 128, 128, 128}));
    for (const unsigned bits : {24, 32}) {
        const auto limit = std::int64_t{1} << (bits - 1);
        Bytes source, expected;
        for (const auto value : {-limit, -limit / 2, std::int64_t{-100}, std::int64_t{0},
             std::int64_t{100}, limit / 2, limit - 1}) append(source, value, bits / 8);
        for (const auto value : {-limit, -limit, std::int64_t{-200}, std::int64_t{0},
             std::int64_t{200}, limit - 1, limit - 1}) append(expected, value, bits / 8);
        CHECK(applyWaveGain(wave(bits, source), 200) == wave(bits, expected));
    }
}

Bytes floats(std::initializer_list<float> samples) {
    Bytes bytes;
    for (const float sample : samples) {
        std::uint32_t raw; std::memcpy(&raw, &sample, sizeof(raw)); append(bytes, raw, 4);
    }
    return bytes;
}
void floatGainRejectsInvalidSamplesAndPreservesUnity() {
    const auto original = wave(32, floats({0, .25f, -.25f, .75f, -.75f, 1, -1}), 3);
    CHECK(applyWaveGain(original, 100) == original);
    CHECK(applyWaveGain(original, 200) == wave(32, floats({0, .5f, -.5f, 1, -1, 1, -1}), 3));
    rejected(wave(32, floats({std::numeric_limits<float>::quiet_NaN()}), 3));
    rejected(wave(32, floats({std::numeric_limits<float>::infinity()}), 3));
    rejected(wave(32, floats({1.5f}), 3));
}

void extensiblePcmAndUnknownChunksKeepTheirBytes() {
    auto original = wave(16, {0xe8, 3});
    const Bytes extension{22,0,16,0,1,0,0,0,1,0,0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
    original.insert(original.begin() + 36, extension.begin(), extension.end());
    put(original, 16, 40, 4); put(original, 20, 0xfffe, 2); put(original, 4, original.size() - 8, 4);
    auto expected = original; expected[68] = 0xd0; expected[69] = 7;
    CHECK(applyWaveGain(original, 200) == expected);
    auto wrong = original; wrong[44] = 7; rejected(wrong);
    wrong = original; wrong[38] = 12; rejected(wrong);
}

void malformedAndOversizedDocumentsAreRejected() {
    const auto good = wave(16, {0, 0});
    rejected(good, -1); rejected(good, 201);
    for (const auto count : {std::size_t{0}, std::size_t{11}, std::size_t{19}, good.size() - 1})
        rejected(Bytes(good.begin(), good.begin() + count));
    for (const auto field : {4, 16, 40}) {
        auto bad = good; put(bad, field, 0xffffffff, 4); rejected(bad);
    }
    for (const auto field : {20, 22, 24, 28, 32, 34}) {
        auto bad = good; put(bad, field, 0, 2); rejected(bad);
    }
    rejected(wave(16, {0}));
    rejected(wave(16, {}));
    rejected(wave(16, Bytes(31 * 1000 * 2, 0)));
    rejected(Bytes(maxNotificationSoundBytes + 1, 0));
    auto duplicate = good;
    duplicate.insert(duplicate.end(), good.begin() + 36, good.end());
    put(duplicate, 4, duplicate.size() - 8, 4); rejected(duplicate);
    auto odd = wave(8, {1}); odd.pop_back(); put(odd, 4, odd.size() - 8, 4); rejected(odd);
}
} // namespace

int main() {
    try {
        pcm16GainSaturatesAndPreservesTheDocument();
        otherPcmWidthsUseCorrectSignednessAndLimits();
        floatGainRejectsInvalidSamplesAndPreservesUnity();
        extensiblePcmAndUnknownChunksKeepTheirBytes();
        malformedAndOversizedDocumentsAreRejected();
        std::cout << "5 audio gain scenarios passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
