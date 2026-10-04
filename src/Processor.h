#pragma once
#include "CloudEngine.h"
#include "Common.h"
#include "Reverb.h"
#include "SpectralEngine.h"

namespace granular {
// input -> delay -> pitched voice stack -> reverb -> tone -> (feedback) back into the delay.
// The pitch shift sits inside the reverb loop, so every pass climbs another interval and gets
// diffused: that recirculation is the shimmer. Freeze mutes the input and holds the loop at the
// level it had when frozen, so a shimmer can sustain indefinitely without running away.
class Processor {
public:
    static constexpr double maxDelaySeconds = 5.0;
    void prepare(double rate);
    void reset() noexcept;
    void setParameters(const Values& values, bool immediate = false) noexcept;
    void setTempo(double bpm) noexcept;
    double tempo() const noexcept { return tempo_; }
    Stereo process(Stereo input) noexcept;
    size_t latency() const noexcept { return spectral_.latency(); }
    float inputPeak() const noexcept { return inputPeak_; }
    float outputPeak() const noexcept { return outputPeak_; }
    float loopEnergy() const noexcept { return loopLevel_; }
private:
    void updateTargets(bool immediate = false) noexcept;

    Values params_ = defaults();
    CloudEngine cloud_;
    SpectralEngine spectral_;
    Reverb reverb_;
    VoiceSet voices_ {}, voiceTarget_ {};

    double rate_ = 48000, tempo_ = 120, smooth_ = .001, characterSmooth_ = .0005;
    double delay_ = 4800, delayTarget_ = 4800;
    float texture_ = 0, textureTarget_ = 0, warp_ = 0, warpTarget_ = 0;
    float character_ = 0, characterTarget_ = 0;
    float inputGain_ = 1, inputGainTarget_ = 1;
    float feedback_ = 0, feedbackTarget_ = 0;
    float mix_ = .35f, mixTarget_ = .35f, gain_ = 1, gainTarget_ = 1, bypass_ = 0, bypassTarget_ = 0;
    float toneCoefficient_ = .5f, toneTarget_ = .5f;
    float spaceMix_ = 0, spaceMixTarget_ = 0, makeup_ = 1, makeupTarget_ = 1;
    float hpCoefficient_ = .995f;
    float safety_ = 1, safetyRelease_ = .0002f;
    Stereo feedbackSample_ {}, lp1_ {}, lp2_ {}, hpInput_ {}, hpOutput_ {};
    // Self-oscillation governor: with the input quiet and the loop not decaying, trim feedback.
    float governor_ = 1, governorApplied_ = 1, inputPower_ = 0, lastLoopPower_ = 0, slowCoefficient_ = .0001f;
    uint32_t checkCounter_ = 0, checkInterval_ = 12000;
    // Freeze AGC
    bool frozen_ = false;
    float loopPower_ = 0, powerCoefficient_ = .001f, heldPower_ = 0, agc_ = 1;
    float inputPeak_ = 0, outputPeak_ = 0, loopLevel_ = 0;
};
} // namespace granular
