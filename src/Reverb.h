#pragma once
#include "Common.h"
#include <array>
#include <vector>

namespace granular {
// Eight-channel orthogonal feedback network with input all-pass diffusion.
// The wet bus feeds this once, after all parallel delay lines have been summed.
class Reverb {
public:
    void prepare(double rate);
    void reset() noexcept;
    void setParameters(double amount, double size, double decay, double tone, bool immediate = false) noexcept;
    Stereo process(Stereo input) noexcept;
private:
    struct Tap {
        std::vector<float> buffer;
        std::vector<uint64_t> epochs;
        uint64_t epoch = 1;
        size_t write = 0;
        float lowpass = 0;
        double delay = 1, targetDelay = 1, feedback = .8, targetFeedback = .8;
        // Slow delay-length modulation smears the network's modes; without it a held tone that
        // lands on a mode rings far above the rest of the spectrum.
        double lfoPhase = 0, lfoStep = 0, modulation = 0;
        float read() const noexcept;
        void push(float sample) noexcept;
        void clear() noexcept;
    };
    struct Allpass {
        std::vector<float> buffer;
        size_t write = 0;
        float process(float input) noexcept;
    };
    struct Network {
        std::array<Tap,8> taps;
        double size = 65;
        void configure(double nextSize, double decay, double rate) noexcept;
        void setDecay(double decay, double rate) noexcept;
        Stereo process(Stereo input, double damping, double smoothing, double modulationDepth) noexcept;
    };
    std::array<Network,2> networks_;
    std::array<std::array<Allpass, 4>, 2> diffusion_;
    double rate_ = 48000, smoothing_ = .001, modulationDepth_ = 0;
    double amount_ = 0, amountTarget_ = 0, damping_ = .5, dampingTarget_ = .5;
    double sizeTarget_ = 65, decayTarget_ = 6, sizeFade_ = 0, sizeFadeStep_ = 0;
    int current_ = 0;
    bool resizing_ = false;
    Stereo dcInput_ {}, dcOutput_ {};
    float dcCoefficient_ = .998f;
};
} // namespace granular
