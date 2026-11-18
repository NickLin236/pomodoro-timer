#include "pomodoro/audio_gain.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace pomodoro {
namespace {
[[noreturn]] void invalid(const char* reason) {
    throw std::runtime_error(reason);
}

std::uint16_t word(const std::vector<std::uint8_t>& bytes, std::size_t at) {
    return static_cast<std::uint16_t>(bytes[at] | (std::uint16_t{bytes[at + 1]} << 8));
}
std::uint32_t dword(const std::vector<std::uint8_t>& bytes, std::size_t at) {
    return std::uint32_t{bytes[at]} | (std::uint32_t{bytes[at + 1]} << 8) |
        (std::uint32_t{bytes[at + 2]} << 16) | (std::uint32_t{bytes[at + 3]} << 24);
}
bool tag(const std::vector<std::uint8_t>& bytes, std::size_t at, const char* text) {
    return std::equal(text, text + 4, bytes.begin() + static_cast<std::ptrdiff_t>(at));
}
} // namespace

std::vector<std::uint8_t> applyWaveGain(const std::vector<std::uint8_t>& wave,
                                     int volumePercent) {
    if (volumePercent < 0 || volumePercent > 200)
        invalid("Notification volume must be between 0% and 200%.");
    if (wave.size() < 12 || wave.size() > maxNotificationSoundBytes)
        invalid("Notification sound is empty, truncated, or larger than 8 MiB.");
    if (!tag(wave, 0, "RIFF") || !tag(wave, 8, "WAVE") ||
        std::uint64_t{dword(wave, 4)} + 8 != wave.size())
        invalid("Notification sound must be a complete RIFF/WAVE file.");

    std::size_t format = 0, formatSize = 0, data = 0, dataSize = 0;
    for (std::size_t at = 12; at < wave.size();) {
        if (wave.size() - at < 8) invalid("Notification sound contains a truncated chunk header.");
        const std::size_t size = dword(wave, at + 4);
        const std::size_t payload = at + 8;
        if (size > wave.size() - payload)
            invalid("Notification sound contains a truncated chunk.");
        if (tag(wave, at, "fmt ")) {
            if (format) invalid("Notification sound contains duplicate format chunks.");
            format = payload; formatSize = size;
        } else if (tag(wave, at, "data")) {
            if (data) invalid("Notification sound contains duplicate audio chunks.");
            data = payload; dataSize = size;
        }
        const std::size_t padded = size + (size & 1);
        if (padded > wave.size() - payload)
            invalid("Notification sound is missing a chunk padding byte.");
        at = payload + padded;
    }
    if (!format || formatSize < 16 || formatSize == 17 || !data || !dataSize)
        invalid("Notification sound is missing a valid format or audio chunk.");
    if (formatSize >= 18 && word(wave, format + 16) > formatSize - 18)
        invalid("Notification sound has a truncated format extension.");

    unsigned codec = word(wave, format);
    const unsigned channels = word(wave, format + 2);
    const std::uint32_t rate = dword(wave, format + 4);
    const std::uint32_t byteRate = dword(wave, format + 8);
    const unsigned frameSize = word(wave, format + 12);
    const unsigned bits = word(wave, format + 14);
    if (codec == 0xfffe) {
        constexpr std::uint8_t subtypeTail[]{0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
        if (formatSize < 40 || word(wave, format + 16) < 22 ||
            word(wave, format + 18) != bits ||
            !std::equal(std::begin(subtypeTail), std::end(subtypeTail),
                wave.begin() + static_cast<std::ptrdiff_t>(format + 28)))
            invalid("Notification sound uses an unsupported extensible audio format.");
        codec = dword(wave, format + 24);
    }
    if (!((codec == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) ||
          (codec == 3 && bits == 32)))
        invalid("Notification sound must use PCM 8/16/24/32-bit or IEEE float32 audio.");
    const unsigned sampleSize = bits / 8;
    if (channels == 0 || channels > 8 || rate == 0 || rate > 384000 ||
        frameSize != channels * sampleSize ||
        std::uint64_t{rate} * frameSize != byteRate || dataSize % frameSize != 0 ||
        dataSize / frameSize > std::uint64_t{rate} * maxNotificationSoundSeconds)
        invalid("Notification sound has invalid audio frames or exceeds 30 seconds.");

    auto result = wave;
    for (std::size_t at = data; at < data + dataSize; at += sampleSize) {
        if (codec == 3) {
            static_assert(sizeof(float) == sizeof(std::uint32_t) && std::numeric_limits<float>::is_iec559);
            auto raw = dword(wave, at);
            float sample;
            std::memcpy(&sample, &raw, sizeof(sample));
            if (!std::isfinite(sample) || sample < -1.0f || sample > 1.0f)
                invalid("Notification sound contains invalid floating-point samples.");
            if (volumePercent == 100) continue;
            sample = static_cast<float>((std::clamp)(double{sample} * volumePercent / 100, -1.0, 1.0));
            std::memcpy(&raw, &sample, sizeof(raw));
            for (unsigned byte = 0; byte < sampleSize; ++byte)
                result[at + byte] = static_cast<std::uint8_t>(raw >> (byte * 8));
            continue;
        }
        if (volumePercent == 100) continue;
        std::int64_t sample = 0;
        if (bits == 8) sample = int{wave[at]} - 128;
        else {
            std::uint64_t raw = 0;
            for (unsigned byte = 0; byte < sampleSize; ++byte)
                raw |= std::uint64_t{wave[at + byte]} << (byte * 8);
            const auto sign = std::uint64_t{1} << (bits - 1);
            sample = raw >= sign ? static_cast<std::int64_t>(raw) - static_cast<std::int64_t>(sign * 2)
                                 : static_cast<std::int64_t>(raw);
        }
        const auto limit = std::int64_t{1} << (bits - 1);
        const auto scaled = (std::clamp)(sample * volumePercent / 100, -limit, limit - 1);
        if (bits == 8) result[at] = static_cast<std::uint8_t>(scaled + 128);
        else {
            const auto raw = static_cast<std::uint64_t>(scaled);
            for (unsigned byte = 0; byte < sampleSize; ++byte)
                result[at + byte] = static_cast<std::uint8_t>(raw >> (byte * 8));
        }
    }
    return result;
}

} // namespace pomodoro
