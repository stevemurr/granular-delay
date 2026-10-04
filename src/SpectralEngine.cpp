#include "SpectralEngine.h"
#include <algorithm>
#include <cmath>

namespace granular {
namespace {
constexpr double pi = 3.14159265358979323846;
constexpr double twoPi = 2*pi;
}

void SpectralEngine::prepare(double rate) {
    rate_ = rate;
    // About 43 ms of latency at any rate; bin spacing then scales with it.
    size_ = 2048;
    while (static_cast<double>(size_) < rate*.04) size_ <<= 1;
    hop_ = size_/4; bins_ = size_/2+1; mask_ = size_-1;
    binHz_ = rate/static_cast<double>(size_);
    fft_.prepare(size_);
    window_.resize(size_);
    for (size_t i = 0; i < size_; ++i)
        window_[i] = static_cast<float>(.5-.5*std::cos(twoPi*static_cast<double>(i)/static_cast<double>(size_)));
    work_.assign(size_, {});
    in_.assign(size_, {}); ola_.assign(size_*2, {});
    olaMask_ = size_*2-1; slice_ = hop_/stages;
    delayBuf_.assign(nextPowerOfTwo(static_cast<size_t>(std::ceil(rate*bufferSeconds))), {});
    delayMask_ = delayBuf_.size()-1;
    for (auto& c : ch_) {
        c.x.assign(bins_, {}); c.y.assign(bins_, {}); c.frozenX.assign(bins_, {});
        for (auto* v : {&c.rawMag, &c.mag, &c.smooth, &c.frozenMag, &c.lastPhase, &c.freq, &c.frozenFreq}) v->assign(bins_, 0.f);
        for (auto* v : {&c.peaks, &c.starts, &c.ends}) { v->clear(); v->reserve(bins_); }
    }
    for (auto& v : voice_) {
        for (auto& p : v.phasor) p.assign(bins_, Cpx {1, 0});
        for (auto& p : v.next) p.assign(bins_, Cpx {1, 0});
        v.scatter.assign(bins_, 0.f);
    }
    reset();
}

void SpectralEngine::reset() noexcept {
    std::fill(delayBuf_.begin(), delayBuf_.end(), Stereo {});
    std::fill(in_.begin(), in_.end(), Stereo {});
    std::fill(ola_.begin(), ola_.end(), Stereo {});
    inWrite_ = delayWrite_ = hopCounter_ = 0; count_ = 0; frameValid_ = false;
    for (auto& c : ch_) {
        for (auto* v : {&c.rawMag, &c.mag, &c.smooth, &c.frozenMag, &c.lastPhase, &c.freq, &c.frozenFreq}) std::fill(v->begin(), v->end(), 0.f);
        std::fill(c.x.begin(), c.x.end(), Cpx {}); std::fill(c.frozenX.begin(), c.frozenX.end(), Cpx {});
    }
    for (auto& v : voice_) {
        for (auto& p : v.phasor) std::fill(p.begin(), p.end(), Cpx {1, 0});
        std::fill(v.scatter.begin(), v.scatter.end(), 0.f);
    }
    rng_ = Rng {};
    wasFrozen_ = false;
}

Stereo SpectralEngine::readDelay() const noexcept {
    const double d = std::clamp(delay_, 2., static_cast<double>(delayBuf_.size()-4));
    // delayWrite_ has already advanced past the current sample, so delay 0 is delayWrite_-1.
    const double pos = static_cast<double>(delayWrite_) - 1. - d;
    const double floorPos = std::floor(pos);
    const float f = static_cast<float>(pos-floorPos);
    const size_t i = static_cast<size_t>(static_cast<int64_t>(floorPos)) & delayMask_;
    const Stereo a = delayBuf_[i], b = delayBuf_[(i+1) & delayMask_];
    return {a.l+(b.l-a.l)*f, a.r+(b.r-a.r)*f};
}

Stereo SpectralEngine::process(Stereo in, const VoiceSet& voices, bool run) noexcept {
    delayBuf_[delayWrite_] = {std::clamp(in.l, -4.f, 4.f), std::clamp(in.r, -4.f, 4.f)};
    delayWrite_ = (delayWrite_+1) & delayMask_;
    in_[inWrite_] = readDelay();
    inWrite_ = (inWrite_+1) & mask_;
    ++count_;
    if (++hopCounter_ == hop_) hopCounter_ = 0;
    // A frame's work is cut into stages, one every slice_ samples, so no single block pays for all
    // of it. Stage 0 fixes which N samples the frame covers.
    if (!run) frameValid_ = false;
    else if (hopCounter_ % slice_ == 0 && hopCounter_/slice_ < stages) {
        const size_t index = hopCounter_/slice_;
        if (index == 0) frameValid_ = true;
        if (frameValid_) stage(index, voices);
    }
    // Position q is complete once every frame covering it has finished its last stage, which is
    // before count_ reaches q + N + hop.
    const size_t read = static_cast<size_t>(count_ - size_ - hop_) & olaMask_;
    const Stereo out = ola_[read];
    ola_[read] = {};
    return run ? out : Stereo {};
}

void SpectralEngine::analyze(Channel& c) noexcept {
    const double omega = twoPi*static_cast<double>(hop_)/static_cast<double>(size_);
    const float a = texture_ < .001f ? 0.f : .99f*std::sqrt(texture_);
    for (size_t k = 0; k < bins_; ++k) {
        const Cpx x = c.x[k];
        const float m = std::sqrt(x.re*x.re+x.im*x.im);
        c.rawMag[k] = m;
        const double phase = std::atan2(static_cast<double>(x.im), static_cast<double>(x.re));
        double d = phase - static_cast<double>(c.lastPhase[k]) - static_cast<double>(k)*omega;
        d -= twoPi*std::round(d/twoPi);
        c.lastPhase[k] = static_cast<float>(phase);
        // Garbage phase differences from a decaying smear must not move a held partial.
        if (a == 0.f || m >= .25f*c.smooth[k]) c.freq[k] = static_cast<float>(static_cast<double>(k) + d/omega);
        // Fast attack, slow release: sustains partials without blurring onsets away.
        c.smooth[k] = m > c.smooth[k] ? c.smooth[k] + (m-c.smooth[k])*(1.f-.5f*a) : c.smooth[k]*a + m*(1.f-a);
    }
    if (a == 0.f) { c.mag = c.rawMag; return; }
    const float blur = .4f*texture_;
    for (size_t k = 0; k < bins_; ++k) {
        const float left = k ? c.smooth[k-1] : 0.f, right = k+1 < bins_ ? c.smooth[k+1] : 0.f;
        const float m = (1.f-blur)*c.smooth[k] + blur*.5f*(left+right);
        c.mag[k] = m;
        const float raw = c.rawMag[k];
        // Re-voice the current frame's phases at the smeared magnitude.
        c.x[k] = raw > 1.e-12f ? c.x[k]*(m/raw) : Cpx {m, 0.f};
    }
}

double SpectralEngine::target(double f, size_t voice, size_t bin, double ratio) const noexcept {
    const double w = warp_;
    switch (warpType_) {
    case 0: { // Stretch: inharmonic, piano-string stiffness. Low partials barely move; upper ones fly out
        const double q = f*binHz_/1500.;
        return ratio*f*std::sqrt(1.+1.5*w*q*q);
    }
    case 1: { // Shift: constant-Hz offset, alternating direction per voice, so harmonics stop being harmonic
        const double sign = voice & 1 ? -1. : 1.;
        return ratio*f + sign*std::pow(w, 1.5)*2000./binHz_;
    }
    case 2: // Scatter: every partial wanders on its own slow random walk
        return ratio*f + static_cast<double>(voice_[voice].scatter[bin])*w*900./binHz_;
    default:
        return ratio*f;
    }
}

void SpectralEngine::synthesize(Channel& c, size_t channel, const VoiceSet& voices) noexcept {
    std::fill(c.y.begin(), c.y.end(), Cpx {});
    const Cpx* x = freeze_ ? c.frozenX.data() : c.x.data();
    const float* mag = freeze_ ? c.frozenMag.data() : c.mag.data();
    const float* freq = freeze_ ? c.frozenFreq.data() : c.freq.data();
    const int last = static_cast<int>(bins_)-2;

    // Regions: runs of bins that agree on one instantaneous frequency (adjacent estimates within
    // 0.6 bin). A lone sinusoid's whole lobe agrees, so it moves as a unit; partials closer than
    // a Hann lobe still separate wherever their estimates part ways. The loudest bin of a run
    // is its representative and carries the phase.
    c.peaks.clear(); c.starts.clear(); c.ends.clear();
    float loudest = 0.f;
    for (int k = 1; k <= last; ++k) loudest = std::max(loudest, mag[k]);
    maxMag_ = loudest;
    const float floorMag = std::max(loudest*1.e-4f, 1.e-3f);
    for (int k = 1; k <= last; ++k) {
        if (mag[k] < floorMag) continue;
        const int start = k;
        int rep = k;
        while (k+1 <= last && mag[k+1] >= floorMag && std::abs(freq[k+1]-freq[k]) < .6f) {
            ++k;
            if (mag[k] > mag[rep]) rep = k;
        }
        c.peaks.push_back(rep); c.starts.push_back(start); c.ends.push_back(k);
    }

    const bool warped = warp_ > .001f;
    const float w = warp_;
    for (size_t v = 0; v < engineVoices; ++v) {
        const float gain = voices.gain[v];
        if (gain < 1.e-5f) continue;
        if (v == 0 && !warped) {
            // Includes DC and Nyquist, which the shifted voices skip, so this path is lossless.
            for (int k = 0; k <= last+1; ++k) c.y[static_cast<size_t>(k)] = c.y[static_cast<size_t>(k)] + x[k]*gain;
            continue;
        }
        auto& state = voice_[v];
        const double ratio = std::pow(2., static_cast<double>(voices.semitones[v])/12.);
        const auto& prev = state.phasor[channel];
        auto& next = state.next[channel];
        next = prev;
        for (size_t i = 0; i < c.peaks.size(); ++i) {
            const int kp = c.peaks[i];
            const double ft = target(static_cast<double>(freq[kp]), v, static_cast<size_t>(kp), ratio);
            if (!(ft >= 1. && ft <= static_cast<double>(last))) continue;
            const int jp = static_cast<int>(std::lround(ft));
            const int shift = jp-kp;
            // The peak's synthesized phase advances by its target frequency each hop.
            double turns = ft*static_cast<double>(hop_)/static_cast<double>(size_);
            turns -= std::floor(turns);
            const float angle = static_cast<float>(twoPi*turns);
            Cpx peak = prev[static_cast<size_t>(jp)] * Cpx {std::cos(angle), std::sin(angle)};
            const float len = std::sqrt(peak.re*peak.re+peak.im*peak.im);
            peak = len > 1.e-12f ? peak*(1.f/len) : Cpx {1, 0};
            const Cpx unit = x[kp]*(1.f/mag[kp]);
            const Cpx rot = peak*conj(unit);
            const int lo = c.starts[i], hi = c.ends[i];
            // Crystal keeps partials and thins the bins between them; quiet partials thin first.
            const float peakGain = warpType_ == 4 ? std::pow(mag[kp]/loudest, .6f*w) : 1.f;
            for (int k = lo; k <= hi; ++k) {
                const int j = k+shift;
                if (j < 1 || j > last) continue;
                const float m = mag[k];
                const Cpx u = m > 1.e-12f ? x[k]*(1.f/m)*rot : rot;
                float g = gain;
                if (warpType_ == 4) g *= std::abs(k-kp) <= 2 ? peakGain : 1.f-w;
                Cpx value = x[k]*rot;
                if (warpType_ == 3) {
                    const float jitter = static_cast<float>(pi)*w*rng_.bipolar();
                    value = value * Cpx {std::cos(jitter), std::sin(jitter)};
                }
                c.y[static_cast<size_t>(j)] = c.y[static_cast<size_t>(j)] + value*g;
                next[static_cast<size_t>(j)] = u;
            }
        }
        state.phasor[channel].swap(next);
    }
}

void SpectralEngine::stage(size_t index, const VoiceSet& voices) noexcept {
    switch (index) {
    case 0: {
        for (size_t i = 0; i < size_; ++i) {
            const Stereo s = in_[(inWrite_+i) & mask_];
            work_[i] = {s.l*window_[i], s.r*window_[i]};
        }
        frameStart_ = static_cast<size_t>(count_ - size_);
        fft_.forward(work_.data());
        // Two real signals in one transform: split by conjugate symmetry.
        for (size_t k = 0; k < bins_; ++k) {
            const Cpx a = work_[k], b = work_[(size_-k) & mask_];
            ch_[0].x[k] = {(a.re+b.re)*.5f, (a.im-b.im)*.5f};
            ch_[1].x[k] = {(a.im+b.im)*.5f, -(a.re-b.re)*.5f};
        }
        break;
    }
    case 1: analyze(ch_[0]); break;
    case 2:
        analyze(ch_[1]);
        if (freeze_ && !wasFrozen_) {
            for (auto& c : ch_) { c.frozenX = c.x; c.frozenMag = c.mag; c.frozenFreq = c.freq; }
        }
        wasFrozen_ = freeze_;
        if (warpType_ == 2 && warp_ > .001f) {
            for (auto& v : voice_) for (auto& s : v.scatter) s = std::clamp(s + .15f*rng_.bipolar(), -1.f, 1.f);
        }
        break;
    case 3: synthesize(ch_[0], 0, voices); break;
    case 4: synthesize(ch_[1], 1, voices); break;
    default: {
        // Repack: Z = YL + i*YR, with each Y completed by conjugate symmetry.
        std::fill(work_.begin(), work_.end(), Cpx {});
        // DC and Nyquist are real for real signals: one real part per channel.
        work_[0] = {ch_[0].y[0].re, ch_[1].y[0].re};
        work_[size_/2] = {ch_[0].y[bins_-1].re, ch_[1].y[bins_-1].re};
        for (size_t j = 1; j+1 < bins_; ++j) {
            const Cpx l = ch_[0].y[j], r = ch_[1].y[j];
            work_[j] = {l.re-r.im, l.im+r.re};
            work_[size_-j] = {l.re+r.im, -l.im+r.re};
        }
        fft_.inverse(work_.data());
        const float scale = 1.f/(static_cast<float>(size_)*1.5f);
        for (size_t i = 0; i < size_; ++i) {
            auto& o = ola_[(frameStart_+i) & olaMask_];
            const float g = window_[i]*scale;
            o.l += work_[i].re*g; o.r += work_[i].im*g;
        }
        break;
    }
    }
}
} // namespace granular
