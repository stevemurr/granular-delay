#pragma once
#include "Parameters.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace granular {
struct Stereo { float l = 0, r = 0; };
inline float clean(float x) noexcept { return std::isfinite(x) ? x : 0.f; }
inline float tiny(float x) noexcept { return std::abs(x) < 1.e-20f ? 0.f : x; }

// Voice 0 is always the unpitched signal; 1..4 are the preset's pitched voices.
inline constexpr size_t engineVoices = maxVoices + 1;
struct VoiceSet {
    std::array<float, engineVoices> semitones {}, gain {};
};

struct Rng {
    uint32_t state = 0x6d2b79f5;
    float next() noexcept { // [0, 1)
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return static_cast<float>(state >> 8) * (1.f / 16777216.f);
    }
    float bipolar() noexcept { return next() * 2.f - 1.f; }
};

inline size_t nextPowerOfTwo(size_t n) noexcept {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}
} // namespace granular
