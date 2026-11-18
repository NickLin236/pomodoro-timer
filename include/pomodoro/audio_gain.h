#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pomodoro {

inline constexpr std::size_t maxNotificationSoundBytes = 8 * 1024 * 1024;
inline constexpr unsigned maxNotificationSoundSeconds = 30;

// Copies a bounded RIFF/WAVE document and changes only its sample bytes.
// Supports PCM 8/16/24/32 and IEEE float32, including their extensible forms.
// Invalid documents, unsupported formats, or gain outside 0..200 throw.
std::vector<std::uint8_t> applyWaveGain(const std::vector<std::uint8_t>& wave,
                                     int volumePercent);

} // namespace pomodoro
