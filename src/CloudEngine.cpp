#include "CloudEngine.h"
#include <algorithm>
#include <cmath>

namespace granular {
namespace {
constexpr double pi = 3.14159265358979323846;
}

void CloudEngine::prepare(double rate) {
    rate_ = rate;
    buffer_.assign(nextPowerOfTwo(static_cast<size_t>(std::ceil(rate*bufferSeconds))), {});
    mask_ = buffer_.size()-1;
    for (size_t i = 0; i < window_.size(); ++i) {
        const double s = std::sin(pi*static_cast<double>(i)/static_cast<double>(window_.size()-1));
        window_[i] = static_cast<float>(s*s); // Hann: two heads at 50% overlap sum to exactly one
    }
    reset();
}

void CloudEngine::reset() noexcept {
    std::fill(buffer_.begin(), buffer_.end(), Stereo {});
    write_ = 0;
    clearGrains();
    rng_ = Rng {};
}

void CloudEngine::clearGrains() noexcept {
    for (auto& voice : voices_) {
        for (auto& grain : voice.grains) grain.length = 0;
        voice.next = 0; voice.live = false;
    }
}

void CloudEngine::record(Stereo in) noexcept {
    buffer_[write_] = {std::clamp(in.l, -4.f, 4.f), std::clamp(in.r, -4.f, 4.f)};
    write_ = (write_+1) & mask_;
}

Stereo CloudEngine::read(double offset) const noexcept {
    // Offsets are kept in [4, size-8] by the grain constraints; the clamp is only a backstop.
    offset = std::clamp(offset, 4., static_cast<double>(buffer_.size()-8));
    // write_ has already advanced past the current sample, so offset 0 is write_-1.
    const double pos = static_cast<double>(write_) - 1. - offset;
    const double floorPos = std::floor(pos);
    const float f = static_cast<float>(pos - floorPos);
    const size_t i1 = static_cast<size_t>(static_cast<int64_t>(floorPos)) & mask_;
    const Stereo y0 = buffer_[(i1-1) & mask_], y1 = buffer_[i1], y2 = buffer_[(i1+1) & mask_], y3 = buffer_[(i1+2) & mask_];
    const auto hermite = [f](float a, float b, float c, float d) {
        const float c1 = .5f*(c-a), c2 = a-2.5f*b+2.f*c-.5f*d, c3 = .5f*(d-a)+1.5f*(b-c);
        return ((c3*f+c2)*f+c1)*f+b;
    };
    return {hermite(y0.l,y1.l,y2.l,y3.l), hermite(y0.r,y1.r,y2.r,y3.r)};
}

float CloudEngine::window(float phase) const noexcept {
    const float index = std::clamp(phase, 0.f, 1.f) * static_cast<float>(window_.size()-1);
    const auto i = static_cast<size_t>(index), j = std::min(i+1, window_.size()-1);
    return window_[i] + (index-static_cast<float>(i)) * (window_[j]-window_[i]);
}

void CloudEngine::trigger(Voice& voice, float semitones) noexcept {
    Grain* slot = nullptr;
    for (auto& g : voice.grains) if (!g.length) { slot = &g; break; }
    if (!slot) return;
    const float t = texture_, w = warp_;
    // Warp: scatter and bend grow quickly past the first third of the knob, so the low end
    // stays musical (slightly loose tuning) and the top end goes properly feral.
    const float w2 = w*w, w3 = w2*w;
    float shift = semitones + rng_.bipolar() * 5.f * w2;
    if (rng_.next() < .5f*w2) shift += rng_.next() < .5f ? 12.f : -12.f;
    const float bend = rng_.bipolar() * 14.f * w3;
    const bool reverse = rng_.next() < .5f*w2;

    const double grainMs = 110. * (1. - .45*t);
    const double length = std::max(64., grainMs*.001*rate_ * (1. + rng_.bipolar()*.3*t));
    double speed = std::pow(2., shift/12.);
    double speedEnd = speed * std::pow(2., bend/12.);
    if (reverse) { speed = -speed; speedEnd = -speedEnd; }
    const double fastest = std::max(speed, speedEnd), slowest = std::min(speed, speedEnd);

    // An offset is the read head's distance behind the write head; it changes by (1 - speed)
    // each sample. Keep it behind the write head, and inside the buffer, for the grain's life.
    const double spray = static_cast<double>(t*t) * .35 * rate_;
    double offset = delay_ + rng_.bipolar()*spray;
    const double minOffset = 4. + std::max(0., fastest-1.) * length;
    const double maxOffset = static_cast<double>(buffer_.size()) - 8. - std::max(0., 1.-slowest) * length;
    offset = std::clamp(offset, minOffset, std::max(minOffset, maxOffset));

    const float pan = rng_.bipolar() * t;
    slot->offset = offset; slot->speed = speed;
    slot->speedMul = std::pow(speedEnd/speed, 1./length);
    slot->age = 0; slot->length = static_cast<uint32_t>(length);
    slot->left = std::sqrt(1.f-pan); slot->right = std::sqrt(1.f+pan);
}

Stereo CloudEngine::render(Voice& voice, float semitones) noexcept {
    voice.next -= 1;
    if (voice.next <= 0) {
        trigger(voice, semitones);
        const float t = texture_;
        const double grainMs = 110. * (1. - .45*t);
        const double overlap = 2. + 5.*t*t;
        voice.next += grainMs*.001*rate_ / overlap * (1. + rng_.bipolar()*.7*t);
    }
    Stereo sum {};
    float weight = 0;
    for (auto& g : voice.grains) {
        if (!g.length) continue;
        const float env = window(static_cast<float>(g.age) / static_cast<float>(std::max<uint32_t>(1, g.length-1)));
        const Stereo s = read(g.offset);
        sum.l += s.l*env*g.left; sum.r += s.r*env*g.right;
        weight += env;
        g.offset += 1. - g.speed;
        g.speed *= g.speedMul;
        if (++g.age >= g.length) g.length = 0;
    }
    const float norm = 1.f / std::max(1.f, weight);
    return {sum.l*norm, sum.r*norm};
}

Stereo CloudEngine::process(Stereo in, const VoiceSet& voices, bool run) noexcept {
    record(in);
    if (!run) {
        if (voices_[0].live || voices_[1].live || voices_[2].live || voices_[3].live || voices_[4].live) clearGrains();
        return {};
    }
    Stereo out {};
    for (size_t v = 0; v < engineVoices; ++v) {
        auto& voice = voices_[v];
        if (voices.gain[v] < 1.e-5f) {
            if (voice.live) { for (auto& g : voice.grains) g.length = 0; voice.live = false; }
            continue;
        }
        voice.live = true;
        const Stereo s = render(voice, voices.semitones[v]);
        out.l += s.l*voices.gain[v]; out.r += s.r*voices.gain[v];
    }
    return out;
}
} // namespace granular
