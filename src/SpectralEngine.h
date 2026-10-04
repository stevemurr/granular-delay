#pragma once
#include "Common.h"
#include "Fft.h"
#include <vector>

namespace granular {
// STFT pitch shifter with rigid peak phase-locking (Laroche & Dolson): every spectral peak
// moves with its neighbouring bins, keeping partial shapes intact. All voices are summed in
// the frequency domain, so one inverse transform serves them all; L and R share one complex
// transform. Warp types bend where each peak lands, or what happens to its phase/level.
//
// Latency is latency() samples. The Processor subtracts it from the delay time so the loop
// stays on the requested time whenever that time is at least this long.
class SpectralEngine {
public:
    static constexpr double bufferSeconds = 8.0;
    void prepare(double rate);
    void reset() noexcept;
    // One frame spans N samples, and its work is spread over the following hop, so output trails
    // the input by N + hop - 1 samples.
    size_t latency() const noexcept { return size_+hop_-1; }
    void setDelay(double samples) noexcept { delay_ = samples; }
    void setWarp(int type, double amount) noexcept { warpType_ = type; warp_ = static_cast<float>(amount); }
    void setTexture(double t) noexcept { texture_ = static_cast<float>(t); }
    void setFreeze(bool freeze) noexcept { freeze_ = freeze; }
    Stereo process(Stereo in, const VoiceSet& voices, bool run) noexcept;
private:
    struct Channel {
        std::vector<Cpx> x, y, frozenX;
        std::vector<float> rawMag, mag, smooth, frozenMag, lastPhase, freq, frozenFreq;
        std::vector<int> peaks, starts, ends;
    };
    struct VoiceState {
        std::array<std::vector<Cpx>, 2> phasor, next;
        std::vector<float> scatter;
    };
    void stage(size_t index, const VoiceSet& voices) noexcept;
    void analyze(Channel& c) noexcept;
    void synthesize(Channel& c, size_t channel, const VoiceSet& voices) noexcept;
    double target(double f, size_t voice, size_t bin, double ratio) const noexcept;
    Stereo readDelay() const noexcept;

    size_t size_ = 2048, hop_ = 512, bins_ = 1025, mask_ = 2047;
    double rate_ = 48000, binHz_ = 23.4375, delay_ = 4800;
    Fft fft_;
    std::vector<float> window_;
    std::vector<Cpx> work_;
    std::vector<Stereo> in_, ola_, delayBuf_;
    size_t inWrite_ = 0, delayWrite_ = 0, delayMask_ = 0, hopCounter_ = 0, olaMask_ = 0, frameStart_ = 0, slice_ = 85;
    uint64_t count_ = 0;
    bool frameValid_ = false;
    static constexpr size_t stages = 6;
    std::array<Channel, 2> ch_;
    std::array<VoiceState, engineVoices> voice_;
    Rng rng_;
    int warpType_ = 0;
    float warp_ = 0, texture_ = 0, maxMag_ = 0;
    bool freeze_ = false, wasFrozen_ = false;
};
} // namespace granular
