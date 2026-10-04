#include "Processor.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace granular {
namespace {
constexpr double pi = 3.14159265358979323846;
}

void Processor::prepare(double rate) {
    if (!std::isfinite(rate) || rate < 8000 || rate > 768000)
        throw std::invalid_argument("Sample rate must be 8000-768000 Hz");
    rate_ = rate;
    smooth_ = 1-std::exp(-1/(.02*rate));
    characterSmooth_ = 1-std::exp(-1/(.04*rate));
    hpCoefficient_ = static_cast<float>(std::exp(-2*pi*40/rate));
    safetyRelease_ = static_cast<float>(1-std::exp(-1/(rate*.1)));
    powerCoefficient_ = static_cast<float>(1-std::exp(-1/(rate*.05)));
    slowCoefficient_ = static_cast<float>(1-std::exp(-1/(rate*.25)));
    checkInterval_ = static_cast<uint32_t>(rate*.25);
    cloud_.prepare(rate);
    spectral_.prepare(rate);
    reverb_.prepare(rate);
    reset();
}

void Processor::reset() noexcept {
    cloud_.reset(); spectral_.reset(); reverb_.reset();
    feedbackSample_ = lp1_ = lp2_ = hpInput_ = hpOutput_ = {};
    loopPower_ = heldPower_ = 0; agc_ = 1; frozen_ = false;
    governor_ = governorApplied_ = 1; inputPower_ = lastLoopPower_ = 0; checkCounter_ = 0;
    inputPeak_ = outputPeak_ = loopLevel_ = 0;
    safety_ = 1;
    setParameters(params_, true);
}

void Processor::setTempo(double bpm) noexcept {
    if (std::isfinite(bpm) && bpm > 0) {
        tempo_ = std::clamp(bpm, 20., 400.);
        updateTargets();
    }
}

void Processor::setParameters(const Values& values, bool immediate) noexcept {
    for (uint32_t i = 0; i < ParamCount; ++i) params_[i] = sanitize(i, values[i]);
    updateTargets(immediate);
    if (immediate) {
        spaceMix_ = spaceMixTarget_; makeup_ = makeupTarget_;
        voices_ = voiceTarget_;
        delay_ = delayTarget_;
        texture_ = textureTarget_; warp_ = warpTarget_; character_ = characterTarget_;
        inputGain_ = inputGainTarget_; feedback_ = feedbackTarget_;
        mix_ = mixTarget_; gain_ = gainTarget_; bypass_ = bypassTarget_;
        toneCoefficient_ = toneTarget_;
    }
}

void Processor::updateTargets(bool immediate) noexcept {
    const bool frozen = params_[Freeze] > .5;
    const double seconds = params_[Sync] > .5
        ? 60./tempo_*divisionBeats[static_cast<size_t>(params_[Division])]
        : params_[Time]*.001;
    delayTarget_ = std::clamp(seconds*rate_, 4., maxDelaySeconds*rate_);

    // Shimmer: equal-power crossfade between the unpitched signal and the pitched stack.
    const double s = params_[Shimmer]*.01;
    const float dry = static_cast<float>(std::cos(s*pi/2)), wet = static_cast<float>(std::sin(s*pi/2));
    const auto& preset = voicePresets[static_cast<size_t>(params_[Voices])];
    float power = 0;
    for (size_t i = 0; i < preset.count; ++i) power += preset.gain[i]*preset.gain[i];
    const float norm = power > 0 ? 1.f/std::sqrt(power) : 0.f;
    voiceTarget_.gain[0] = dry; voiceTarget_.semitones[0] = 0;
    for (size_t i = 0; i < maxVoices; ++i) {
        const bool used = i < preset.count;
        voiceTarget_.gain[i+1] = used ? wet*preset.gain[i]*norm : 0.f;
        voiceTarget_.semitones[i+1] = used ? static_cast<float>(preset.semitones[i]*params_[Interval]*.01) : voices_.semitones[i+1];
    }

    textureTarget_ = static_cast<float>(params_[Texture]*.01);
    warpTarget_ = static_cast<float>(params_[Warp]*.01);
    characterTarget_ = static_cast<float>(params_[Character]);
    inputGainTarget_ = frozen ? 0.f : 1.f;
    mixTarget_ = static_cast<float>(params_[Mix]*.01);
    gainTarget_ = static_cast<float>(std::pow(10., params_[Output]/20.));
    bypassTarget_ = static_cast<float>(params_[Bypass]);
    toneTarget_ = static_cast<float>(1-std::exp(-2*pi*std::min(params_[Tone], rate_*.45)/rate_));

    // Space: the reverb sits in the loop fully wet and is blended in here. Its level rises over the
    // first 60% of the knob; decay runs 0.3-18 s throughout. Noise comes out of the reverb well
    // below the level it went in (about -14 dB at short decays, -6 dB at 18 s), so the wet output
    // (never the loop, whose gain must stay below one) gets makeup to land near unity.
    const double space = params_[Space]*.01;
    const double decay = frozen ? 30. : .3*std::pow(60., space);
    spaceMixTarget_ = static_cast<float>(std::min(1., space/.6));
    // Bloom's ceiling. A held tone that lands on a reverb mode comes back up to ~0.7*(decay/0.5)^0.39
    // times larger (measured, modulation on), and a pure-tone Spectral pass at Shimmer 0 returns it
    // at unity. Scaling the feedback by the inverse puts the loop gain at the worst-case mode at
    // exactly Bloom: below 100% every tail dies away whatever Space is; 100% is the edge of sustain.
    const double worstTone = .71*std::pow(decay/.5, .39);
    feedbackTarget_ = static_cast<float>(params_[Bloom]*.01*std::min(.985, 1./worstTone));
    makeupTarget_ = static_cast<float>(std::min(6., 1./(.246*std::pow(decay, .238))));
    reverb_.setParameters(100, 80, decay, std::min(params_[Tone]*1.1, 16000.), immediate);
    spectral_.setWarp(static_cast<int>(params_[WarpType]), warpTarget_);
    spectral_.setFreeze(frozen);
}

Stereo Processor::process(Stereo input) noexcept {
    input = {clean(input.l), clean(input.r)};
    const auto follow = [this](float& current, float target) {
        // Single-precision one-poles stall just shy of their endpoints; snap when close.
        if (std::abs(target-current) < .0001f) current = target;
        else current += static_cast<float>(smooth_)*(target-current);
    };
    follow(mix_, mixTarget_); follow(gain_, gainTarget_); follow(bypass_, bypassTarget_);
    follow(inputGain_, inputGainTarget_); follow(feedback_, feedbackTarget_);
    follow(texture_, textureTarget_); follow(warp_, warpTarget_);
    follow(toneCoefficient_, toneTarget_);
    follow(spaceMix_, spaceMixTarget_); follow(makeup_, makeupTarget_);
    if (std::abs(characterTarget_-character_) < .0005f) character_ = characterTarget_;
    else character_ += static_cast<float>(characterSmooth_)*(characterTarget_-character_);
    delay_ += smooth_*(delayTarget_-delay_);
    for (size_t v = 0; v < engineVoices; ++v) {
        follow(voices_.gain[v], voiceTarget_.gain[v]);
        follow(voices_.semitones[v], voiceTarget_.semitones[v]);
    }

    const bool freeze = params_[Freeze] > .5;
    if (freeze != frozen_) {
        frozen_ = freeze;
        // Hold the loop at the power it had at the instant of freezing.
        if (freeze) heldPower_ = loopPower_;
    }

    const bool runCloud = character_ < .9995f, runSpectral = character_ > .0005f;
    const Stereo loopIn {input.l*inputGain_+feedbackSample_.l, input.r*inputGain_+feedbackSample_.r};
    cloud_.setDelay(delay_); cloud_.setTexture(texture_); cloud_.setWarp(warp_);
    spectral_.setDelay(std::max(delay_-static_cast<double>(spectral_.latency()), 4.));
    spectral_.setTexture(texture_);
    const Stereo a = cloud_.process(loopIn, voices_, runCloud);
    const Stereo b = spectral_.process(loopIn, voices_, runSpectral);
    const float c = std::clamp(character_, 0.f, 1.f);
    const Stereo voiced {a.l+(b.l-a.l)*c, a.r+(b.r-a.r)*c};

    const Stereo room = reverb_.process(voiced);
    const Stereo diffuse {voiced.l*(1-spaceMix_)+room.l*spaceMix_, voiced.r*(1-spaceMix_)+room.r*spaceMix_};
    lp1_.l = tiny(lp1_.l+toneCoefficient_*(diffuse.l-lp1_.l)); lp1_.r = tiny(lp1_.r+toneCoefficient_*(diffuse.r-lp1_.r));
    lp2_.l = tiny(lp2_.l+toneCoefficient_*(lp1_.l-lp2_.l)); lp2_.r = tiny(lp2_.r+toneCoefficient_*(lp1_.r-lp2_.r));
    const Stereo wet {tiny(lp2_.l-hpInput_.l+hpCoefficient_*hpOutput_.l), tiny(lp2_.r-hpInput_.r+hpCoefficient_*hpOutput_.r)};
    hpInput_ = lp2_; hpOutput_ = wet;

    const float power = .5f*(wet.l*wet.l+wet.r*wet.r);
    const float heard = 1.f+spaceMix_*(makeup_-1.f);
    loopPower_ += powerCoefficient_*(power-loopPower_);
    inputPower_ += slowCoefficient_*(.5f*(input.l*input.l+input.r*input.r)-inputPower_);
    if (++checkCounter_ >= checkInterval_) {
        checkCounter_ = 0;
        // Healthy tails fall at least ~3 dB/s even at an 18 s decay. A quiet input over a loop that
        // falls slower than 1.5 dB/s is ringing on a reverb mode: pull the feedback down until it dies.
        const bool quiet = inputPower_ < .05f*loopPower_;
        const bool sustaining = loopPower_ > 1.e-9f && loopPower_ > lastLoopPower_*.917f;
        if (!freeze && quiet && sustaining) governor_ *= .9f;
        else governor_ += .02f*(1.f-governor_);
        lastLoopPower_ = loopPower_;
    }
    // Bloom at the very top of its range asks for infinite sustain; leave that alone.
    governorApplied_ += .002f*((params_[Bloom] >= 97. ? 1.f : governor_)-governorApplied_);
    float gain = feedback_*governorApplied_;
    if (freeze) {
        // Slow AGC toward the held power: sustains through tone/reverb losses, never grows.
        const float wanted = loopPower_ > 1.e-12f ? std::sqrt(heldPower_/loopPower_) : 1.f;
        agc_ += .0005f*(std::clamp(wanted, .5f, 4.f)-agc_);
        gain = agc_;
    } else agc_ += .002f*(1.f-agc_);
    // Soft ceiling inside the loop only; linear well below the clip.
    const auto soft = [](float x) { return 2.f*std::tanh(.5f*x); };
    feedbackSample_ = {clean(soft(wet.l*gain)), clean(soft(wet.r*gain))};

    Stereo out {(input.l*(1-mix_)+wet.l*heard*mix_)*gain_, (input.r*(1-mix_)+wet.r*heard*mix_)*gain_};
    // Stereo-linked ceiling on the processed signal: instant attack, smooth recovery.
    const float peak = std::max(std::abs(out.l), std::abs(out.r));
    const float limit = peak > .98f ? .98f/peak : 1.f;
    safety_ = std::min(limit, safety_+safetyRelease_*(1-safety_));
    out.l *= safety_; out.r *= safety_;
    if (bypass_ >= 1.f) out = input;
    else { out.l += (input.l-out.l)*bypass_; out.r += (input.r-out.r)*bypass_; }

    inputPeak_ = std::max(std::max(std::abs(input.l), std::abs(input.r)), inputPeak_*.9998f);
    outputPeak_ = std::max(std::max(std::abs(out.l), std::abs(out.r)), outputPeak_*.9998f);
    loopLevel_ = std::sqrt(loopPower_);
    return {clean(out.l), clean(out.r)};
}
} // namespace granular
