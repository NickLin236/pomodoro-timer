#pragma once

#include <array>
#include <functional>
#include <string>

namespace pomodoro {

using SessionEntropy = std::array<unsigned char, 16>;
using SessionEntropySource = std::function<SessionEntropy()>;

// Draws 128 bits from the operating system CSPRNG. Failure throws; callers must
// not replace failed entropy with a clock, counter, or deterministic PRNG.
SessionEntropy secureSessionEntropy();
std::string encodeSessionId(const SessionEntropy& entropy);

} // namespace pomodoro
