#pragma once
#include "Common.h"
#include <vector>

namespace granular {
// Granular pitch shifter. Each voice is its own grain stream reading the loop's delay line.
// Texture 0 is the classic smooth two-head shifter; higher Texture adds spray, density and
// stereo scatter. Warp adds per-grain pitch scatter, octave jumps, bends and reversal.
class CloudEngine {
public:
    static constexpr size_t maxGrains = 12;
    static constexpr double bufferSeconds = 8.0; // 5 s of delay plus spray and pitch travel
    void prepare(double rate);
    void reset() noexcept;
    void setDelay(double samples) noexcept { delay_ = samples; }
    void setTexture(double t) noexcept { texture_ = static_cast<float>(t); }
    void setWarp(double w) noexcept { warp_ = static_cast<float>(w); }
    // `run` false keeps the delay line fed without spending time on grains.
    Stereo process(Stereo in, const VoiceSet& voices, bool run) noexcept;
private:
    struct Grain {
        double offset = 0, speed = 1, speedMul = 1;
        uint32_t age = 0, length = 0;
        float left = 1, right = 1;
    };
    struct Voice {
        std::array<Grain, maxGrains> grains {};
        double next = 0;
        bool live = false;
    };
    void record(Stereo in) noexcept;
    Stereo read(double offset) const noexcept;
    float window(float phase) const noexcept;
    void trigger(Voice& voice, float semitones) noexcept;
    Stereo render(Voice& voice, float semitones) noexcept;
    void clearGrains() noexcept;

    std::vector<Stereo> buffer_;
    size_t mask_ = 0, write_ = 0;
    std::array<Voice, engineVoices> voices_ {};
    std::array<float, 1025> window_ {};
    Rng rng_;
    double rate_ = 48000, delay_ = 4800;
    float texture_ = 0, warp_ = 0;
};
} // namespace granular
