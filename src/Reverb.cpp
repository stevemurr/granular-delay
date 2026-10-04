#include "Reverb.h"
#include <cmath>

namespace granular {
namespace {
constexpr std::array<double, 8> delays {.0311, .0379, .0437, .0533, .0617, .0719, .0839, .0971};
float scrub(float x) { return std::isfinite(x) && std::abs(x) >= 1.e-20f ? x : 0; }
double bounded(double x, double low, double high, double fallback) {
    return std::isfinite(x) ? std::clamp(x,low,high) : fallback;
}
}
float Reverb::Tap::read() const noexcept {
    double pos = static_cast<double>(write)-delay-modulation;
    if (pos < 0) pos += buffer.size();
    const size_t i = static_cast<size_t>(pos), j = (i+1)%buffer.size();
    const float a = epochs[i] == epoch ? buffer[i] : 0, b = epochs[j] == epoch ? buffer[j] : 0;
    return a+static_cast<float>(pos-i)*(b-a);
}
void Reverb::Tap::push(float sample) noexcept {
    buffer[write] = scrub(sample); epochs[write] = epoch;
    if (++write == buffer.size()) write = 0;
}
void Reverb::Tap::clear() noexcept { ++epoch; write = 0; lowpass = 0; }
float Reverb::Allpass::process(float input) noexcept {
    const float delayed = buffer[write];
    const float output = scrub(delayed-.65f*input);
    buffer[write] = scrub(input+.65f*output);
    if (++write == buffer.size()) write = 0;
    return output;
}
void Reverb::Network::configure(double nextSize, double decay, double rate) noexcept {
    size = nextSize;
    const double scale = .5+size*.015;
    for (size_t i = 0; i < taps.size(); ++i) {
        auto& tap = taps[i]; tap.clear();
        tap.delay = tap.targetDelay = delays[i]*scale*rate;
        tap.feedback = tap.targetFeedback = std::pow(10.,-3*tap.delay/rate/decay);
    }
}
void Reverb::Network::setDecay(double decay, double rate) noexcept {
    for (auto& tap : taps) tap.targetFeedback = std::pow(10.,-3*tap.delay/rate/decay);
}
Stereo Reverb::Network::process(Stereo input, double damping, double smoothing, double modulationDepth) noexcept {
    std::array<float,8> delayed {};
    float sum = 0;
    for (size_t i = 0; i < taps.size(); ++i) {
        auto& tap = taps[i];
        tap.lfoPhase += tap.lfoStep; if (tap.lfoPhase >= 6.283185307179586) tap.lfoPhase -= 6.283185307179586;
        tap.modulation = modulationDepth*std::sin(tap.lfoPhase);
        tap.feedback += smoothing*(tap.targetFeedback-tap.feedback);
        tap.lowpass = scrub(tap.lowpass+static_cast<float>(damping)*(tap.read()-tap.lowpass));
        delayed[i] = tap.lowpass; sum += delayed[i];
    }
    Stereo wet {};
    constexpr float leftSigns[] {1,1,-1,-1,1,1,-1,-1};
    constexpr float rightSigns[] {1,-1,-1,1,1,-1,-1,1};
    for (size_t i = 0; i < taps.size(); ++i) {
        const float excitation = .25f*(leftSigns[i]*input.l+rightSigns[i]*input.r);
        const float recirculated = static_cast<float>(taps[i].feedback)*(delayed[i]-.25f*sum);
        taps[i].push(excitation+recirculated);
        wet.l += delayed[i]*leftSigns[i]*.35f;
        wet.r += delayed[i]*rightSigns[i]*.35f;
    }
    return {scrub(wet.l),scrub(wet.r)};
}
void Reverb::prepare(double rate) {
    rate_ = rate; smoothing_ = 1-std::exp(-1/(rate*.04));
    sizeFadeStep_ = 1/(rate*.2);
    modulationDepth_ = rate*.0007; // +/-0.7 ms
    for (size_t n = 0; n < networks_.size(); ++n) for (size_t i = 0; i < 8; ++i) {
        auto& tap = networks_[n].taps[i];
        tap.lfoStep = 6.283185307179586*(.11+.037*static_cast<double>(i)+.021*static_cast<double>(n))/rate;
        tap.lfoPhase = 6.283185307179586*static_cast<double>(i)/8.;
    }
    dcCoefficient_ = static_cast<float>(std::exp(-6.283185307179586*15/rate));
    for (auto& network : networks_) for (size_t i = 0; i < network.taps.size(); ++i) {
        auto& tap = network.taps[i];
        tap.buffer.assign(static_cast<size_t>(std::ceil(delays[i]*2*rate))+4+static_cast<size_t>(rate*.004),0);
        tap.epochs.assign(tap.buffer.size(),0); tap.epoch = 1;
    }
    constexpr double times[2][4] {{.0047,.0071,.0097,.0127}, {.0053,.0079,.0109,.0137}};
    for (size_t c = 0; c < 2; ++c) for (size_t i = 0; i < 4; ++i)
        diffusion_[c][i].buffer.assign(std::max(size_t(1), static_cast<size_t>(times[c][i]*rate)), 0);
    reset();
}
void Reverb::reset() noexcept {
    dcInput_ = dcOutput_ = {}; current_ = 0; resizing_ = false; sizeFade_ = 0;
    for (auto& network : networks_) network.configure(sizeTarget_,decayTarget_,rate_);
    for (auto& channel : diffusion_) for (auto& stage : channel) { std::fill(stage.buffer.begin(),stage.buffer.end(),0); stage.write = 0; }
}
void Reverb::setParameters(double amount, double size, double decay, double tone, bool immediate) noexcept {
    amountTarget_ = bounded(amount,0,100,0)*.01;
    sizeTarget_ = bounded(size,0,100,65); decayTarget_ = bounded(decay,.2,30,6);
    dampingTarget_ = 1-std::exp(-6.283185307179586*std::min(bounded(tone,500,16000,6000),rate_*.45)/rate_);
    for (auto& network : networks_) network.setDecay(decayTarget_,rate_);
    if (immediate) {
        amount_ = amountTarget_; damping_ = dampingTarget_; current_ = 0; resizing_ = false;
        for (auto& network : networks_) network.configure(sizeTarget_,decayTarget_,rate_);
    }
}
Stereo Reverb::process(Stereo input) noexcept {
    input = {std::clamp(scrub(input.l),-32.f,32.f),std::clamp(scrub(input.r),-32.f,32.f)};
    amount_ += smoothing_*(amountTarget_-amount_);
    damping_ += smoothing_*(dampingTarget_-damping_);
    Stereo diffused {scrub(input.l-dcInput_.l+dcCoefficient_*dcOutput_.l),scrub(input.r-dcInput_.r+dcCoefficient_*dcOutput_.r)};
    dcInput_ = input; dcOutput_ = diffused;
    for (auto& stage : diffusion_[0]) diffused.l = stage.process(diffused.l);
    for (auto& stage : diffusion_[1]) diffused.r = stage.process(diffused.r);
    // Moving read heads can inject energy into a high-feedback FDN. Each
    // network instead keeps fixed delays; Size crossfades independent networks.
    // Epochs clear the incoming network in O(1) per tap without allocation.
    if (!resizing_ && std::abs(networks_[current_].size-sizeTarget_) > .01) {
        networks_[1-current_].configure(sizeTarget_,decayTarget_,rate_);
        resizing_ = true; sizeFade_ = 0;
    }
    Stereo wet = networks_[current_].process(diffused,damping_,smoothing_,modulationDepth_);
    if (resizing_) {
        const auto next = networks_[1-current_].process(diffused,damping_,smoothing_,modulationDepth_);
        const float x = static_cast<float>(std::min(1.,sizeFade_));
        const float weight = x*x*(3-2*x);
        wet.l += (next.l-wet.l)*weight; wet.r += (next.r-wet.r)*weight;
        sizeFade_ += sizeFadeStep_;
        if (sizeFade_ >= 1) { current_ = 1-current_; resizing_ = false; }
    }
    if (amount_ < 1.e-8 && amountTarget_ == 0) return input;
    const float mix = static_cast<float>(amount_);
    return {scrub(input.l+(wet.l-input.l)*mix),scrub(input.r+(wet.r-input.r)*mix)};
}
} // namespace granular
