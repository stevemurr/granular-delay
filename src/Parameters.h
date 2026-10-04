#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace granular {
// One engine, a handful of macros. IDs are persistent: append, never reorder.
enum Param : uint32_t {
    Character,  // Cloud (granular shifter) or Spectral (STFT)
    Voices,     // interval stack preset
    Interval,   // scales the stack: 100% = as written, 0% = unison, 200% = doubled
    Warp,       // Cloud: pitch wildness. Spectral: amount of the selected warp
    WarpType,   // Spectral only
    Texture,    // Cloud: smooth shimmer -> scattered grains. Spectral: spectral smear
    Shimmer,    // unpitched <-> pitched voices inside the loop
    Time,       // ms, when Sync is off
    Sync,
    Division,   // when Sync is on
    Bloom,      // feedback
    Space,      // reverb amount and decay inside the loop
    Tone,       // low-pass inside the loop
    Freeze,
    Mix,
    Output,
    Bypass,
    ParamCount
};
struct ParamInfo {
    const char* name;
    const char* unit;
    double min, max, initial;
    bool stepped;
    bool log;
};
inline constexpr std::array<ParamInfo, ParamCount> parameters {{
    {"Character", "", 0, 1, 0, true, false},
    {"Voices", "", 0, 12, 0, true, false},
    {"Interval", "%", 0, 200, 100, false, false},
    {"Warp", "%", 0, 100, 0, false, false},
    {"Warp Type", "", 0, 4, 0, true, false},
    {"Texture", "%", 0, 100, 25, false, false},
    {"Shimmer", "%", 0, 100, 55, false, false},
    {"Time", "ms", 20, 2000, 120, false, true},
    {"Tempo Sync", "", 0, 1, 0, true, false},
    {"Division", "", 0, 8, 2, true, false},
    {"Bloom", "%", 0, 100, 55, false, false},
    {"Space", "%", 0, 100, 70, false, false},
    {"Tone", "Hz", 800, 18000, 7000, false, true},
    {"Freeze", "", 0, 1, 0, true, false},
    {"Mix", "%", 0, 100, 35, false, false},
    {"Output", "dB", -24, 12, 0, false, false},
    {"Bypass", "", 0, 1, 0, true, false},
}};

// CHOMPI's nine divisions, expressed as quarter-note beats.
inline constexpr std::array<double, 9> divisionBeats {0.5, 2.0/3.0, 1, 4.0/3.0, 1.5, 2, 3, 4, 8};
inline constexpr std::array<const char*, 9> divisionNames {
    "1/8", "1/4 triplet", "1/4", "1/2 triplet", "1/4 dotted", "1/2", "1/2 dotted", "1 bar", "2 bars"
};
inline constexpr std::array<const char*, 2> characterNames {"Cloud", "Spectral"};
inline constexpr std::array<const char*, 5> warpNames {"Stretch", "Shift", "Scatter", "Whisper", "Crystal"};

inline constexpr size_t maxVoices = 4;
struct VoicePreset {
    const char* name;
    size_t count;
    std::array<float, maxVoices> semitones, gain;
};
// Pitched voices only; the Shimmer knob crossfades them against the unpitched signal.
inline constexpr std::array<VoicePreset, 13> voicePresets {{
    {"Octave",         1, {12, 0, 0, 0},       {1, 0, 0, 0}},
    {"Fifth",          1, {7, 0, 0, 0},        {1, 0, 0, 0}},
    {"Octave + Fifth", 2, {12, 19, 0, 0},      {1, .7f, 0, 0}},
    {"Two Octaves",    2, {12, 24, 0, 0},      {1, .6f, 0, 0}},
    {"Up & Down",      2, {12, -12, 0, 0},     {1, .8f, 0, 0}},
    {"Major",          3, {12, 16, 19, 0},     {1, .7f, .6f, 0}},
    {"Minor",          3, {12, 15, 19, 0},     {1, .7f, .6f, 0}},
    {"Sus",            3, {12, 17, 19, 0},     {1, .7f, .6f, 0}},
    {"Sub",            1, {-12, 0, 0, 0},      {1, 0, 0, 0}},
    {"Deep",           2, {-12, -24, 0, 0},    {1, .7f, 0, 0}},
    {"Stack",          4, {7, 12, 19, 24},     {.6f, 1, .6f, .5f}},
    {"Detune",         2, {.14f, -.14f, 0, 0}, {1, 1, 0, 0}},
    {"Cluster",        4, {-5, 1, 6, 13},      {.7f, 1, .7f, .6f}},
}};

using Values = std::array<double, ParamCount>;
inline Values defaults() noexcept {
    Values values {};
    for (size_t i = 0; i < values.size(); ++i) values[i] = parameters[i].initial;
    return values;
}
inline double sanitize(uint32_t id, double value) noexcept {
    if (id >= ParamCount) return 0;
    const auto& p = parameters[id];
    if (!std::isfinite(value)) value = p.initial;
    value = std::clamp(value, p.min, p.max);
    return p.stepped ? std::trunc(value) : value;
}
// Knob position <-> value. Log parameters are exponential across their range.
inline double toNormalized(uint32_t id, double value) noexcept {
    const auto& p = parameters[id];
    value = std::clamp(value, p.min, p.max);
    return p.log ? std::log(value/p.min)/std::log(p.max/p.min) : (value-p.min)/(p.max-p.min);
}
inline double fromNormalized(uint32_t id, double position) noexcept {
    const auto& p = parameters[id];
    position = std::clamp(position, 0., 1.);
    return p.log ? p.min*std::pow(p.max/p.min, position) : p.min+(p.max-p.min)*position;
}
inline const char* paramLabel(uint32_t id, int value) {
    switch (id) {
    case Character: return characterNames[static_cast<size_t>(std::clamp(value, 0, 1))];
    case Voices: return voicePresets[static_cast<size_t>(std::clamp(value, 0, 12))].name;
    case WarpType: return warpNames[static_cast<size_t>(std::clamp(value, 0, 4))];
    case Division: return divisionNames[static_cast<size_t>(std::clamp(value, 0, 8))];
    default: return value ? "On" : "Off";
    }
}
} // namespace granular
