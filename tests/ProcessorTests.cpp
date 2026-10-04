#include "Fft.h"
#include "Processor.h"
#include "Test.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace granular;
constexpr double pi = 3.14159265358979323846;

struct Rig {
    Processor dsp;
    Values p = defaults();
    double rate;
    explicit Rig(double sampleRate = 48000) : rate(sampleRate) { dsp.setParameters(p); dsp.prepare(rate); }
    void set(Param id, double v) { p[id] = v; dsp.setParameters(p); }
    Stereo step(Stereo in) { return dsp.process(in); }
};
double rms(const std::vector<float>& x, size_t from, size_t to) {
    double sum = 0;
    for (size_t i = from; i < to; ++i) sum += static_cast<double>(x[i])*x[i];
    return std::sqrt(sum/static_cast<double>(to-from));
}
// Magnitude near hz in the Hann-windowed spectrum of x[from, from+n).
double band(const std::vector<float>& x, size_t from, size_t n, double hz, double rate, double width = 20) {
    Fft fft; fft.prepare(n);
    std::vector<Cpx> w(n);
    for (size_t i = 0; i < n; ++i)
        w[i] = {x[from+i]*static_cast<float>(.5-.5*std::cos(2*pi*static_cast<double>(i)/static_cast<double>(n))), 0};
    fft.forward(w.data());
    double m = 0;
    for (size_t k = 1; k < n/2; ++k)
        if (std::abs(static_cast<double>(k)*rate/static_cast<double>(n)-hz) < width) m = std::max(m, std::hypot(double(w[k].re), double(w[k].im))*4/static_cast<double>(n));
    return m;
}
double sineAt(double hz, size_t i, double rate) { return std::sin(2*pi*hz*static_cast<double>(i)/rate); }

void dryAndBypassAreExact() {
    for (int mode : {0, 1}) {
        Rig rig;
        rig.set(Mix, 0); rig.set(Character, mode);
        Rng rng;
        for (int i = 0; i < 20000; ++i) {
            const Stereo in {.4f*rng.bipolar(), .4f*rng.bipolar()};
            const auto out = rig.step(in);
            if (i > 12000) CHECK(out.l == in.l && out.r == in.r); // after the 20 ms smoothing has snapped to its target
        }
        rig.set(Mix, 100); rig.set(Bypass, 1);
        for (int i = 0; i < 20000; ++i) rig.step({.4f*rng.bipolar(), 0.f});
        const Stereo in {.3f, -.2f};
        const auto out = rig.step(in);
        CHECK(out.l == in.l && out.r == in.r);
    }
}

// Cloud and Spectral both land the first echo on the requested time, synced or not.
void echoLandsOnTime() {
    for (int mode : {0, 1}) {
        for (int sync : {0, 1}) {
            Rig rig;
            rig.set(Character, mode); rig.set(Mix, 100); rig.set(Shimmer, 0); rig.set(Space, 0);
            rig.set(Bloom, 0); rig.set(Texture, 0); rig.set(Tone, 18000);
            rig.set(Sync, sync); rig.set(Time, 500); rig.set(Division, 2); // 1/4 at 120 bpm = 500 ms
            rig.dsp.setTempo(120);
            std::vector<float> out(48000);
            for (size_t i = 0; i < out.size(); ++i) out[i] = rig.step({i == 100 ? .8f : 0.f, 0.f}).l;
            size_t peak = 0;
            for (size_t i = 0; i < out.size(); ++i) if (std::abs(out[i]) > std::abs(out[peak])) peak = i;
            const double ms = (static_cast<double>(peak)-100)/48.;
            std::printf("%s, sync %d: echo at %.2f ms (peak %.3f)\n", mode ? "Spectral" : "Cloud", sync, ms, out[peak]);
            CHECK(std::abs(ms-500) < 6);
        }
    }
}

// Shimmer: a decaying 220 Hz note keeps climbing in octaves through the loop.
void shimmerCascades() {
    for (int mode : {0, 1}) {
        Rig rig;
        rig.set(Character, mode); rig.set(Mix, 100); rig.set(Shimmer, 100); rig.set(Voices, 0);
        rig.set(Bloom, 80); rig.set(Space, 40); rig.set(Time, 150); rig.set(Texture, 0); rig.set(Tone, 12000);
        std::vector<float> out(48000*6);
        for (size_t i = 0; i < out.size(); ++i) {
            const float x = i < 24000 ? static_cast<float>(.4*sineAt(220, i, 48000)) : 0.f;
            out[i] = rig.step({x, x}).l;
        }
        // A 220 Hz note shimmers through 440, 880, 1760 ... one pass at a time.
        const size_t from = 48000, n = 32768;
        const double a440 = band(out, from, n, 440, 48000, 30), a880 = band(out, from, n, 880, 48000, 40);
        const double a1760 = band(out, from, n, 1760, 48000, 60), a220 = band(out, from, n, 220, 48000, 20);
        std::printf("%s shimmer tail: 220 Hz %.4f, 440 Hz %.4f, 880 Hz %.4f, 1760 Hz %.4f\n", mode ? "Spectral" : "Cloud", a220, a440, a880, a1760);
        CHECK(a440 > a220*3 && a880 > 0.002);
        float peak = 0;
        for (auto x : out) peak = std::max(peak, std::abs(x));
        CHECK(peak <= .981f);
    }
}

// Freeze sustains a held level in either character, then releases back to live audio.
void freezeSustains() {
    for (int mode : {0, 1}) {
        Rig rig;
        rig.set(Character, mode); rig.set(Mix, 100); rig.set(Shimmer, 60); rig.set(Voices, 5);
        rig.set(Bloom, 40); rig.set(Space, 50); rig.set(Time, 200); rig.set(Texture, 20);
        std::vector<float> out(48000*14);
        for (size_t i = 0; i < out.size(); ++i) {
            if (i == 48000*2) rig.set(Freeze, 1);
            const float x = i < 48000*2 ? static_cast<float>(.3*sineAt(330, i, 48000)+.2*sineAt(495, i, 48000)) : 0.f;
            out[i] = rig.step({x, x}).l;
        }
        const double early = rms(out, 48000*3, 48000*4), late = rms(out, 48000*12, 48000*14);
        std::printf("%s freeze: rms %.4f at 1 s, %.4f at 10 s\n", mode ? "Spectral" : "Cloud", early, late);
        CHECK(early > .02);
        CHECK(late > early*.5 && late < early*2.);
        float peak = 0;
        for (auto x : out) { CHECK(std::isfinite(x)); peak = std::max(peak, std::abs(x)); }
        CHECK(peak <= .981f);
    }
    // Releasing: with the input still silent, the held sound dies away with Bloom/Space.
    Rig rig;
    rig.set(Mix, 100); rig.set(Bloom, 40); rig.set(Space, 30); rig.set(Time, 120);
    std::vector<float> out(48000*12);
    for (size_t i = 0; i < out.size(); ++i) {
        if (i == 48000) rig.set(Freeze, 1);
        if (i == 48000*5) rig.set(Freeze, 0);
        const float x = i < 48000 ? static_cast<float>(.4*sineAt(330, i, 48000)) : 0.f;
        out[i] = rig.step({x, x}).l;
    }
    const double held = rms(out, 48000*3, 48000*5), gone = rms(out, 48000*11, 48000*12);
    std::printf("release: held %.4f -> %.6f six seconds later\n", held, gone);
    CHECK(held > .02 && gone < held*.05);
}

// The loop cannot run away: full Bloom on loud noise, then silence, stays bounded.
void fullBloomStaysBounded() {
    for (int mode : {0, 1}) {
        Rig rig;
        rig.set(Character, mode); rig.set(Bloom, 100); rig.set(Shimmer, 100); rig.set(Voices, 10);
        rig.set(Space, 100); rig.set(Mix, 100); rig.set(Tone, 18000); rig.set(Time, 40);
        Rng rng;
        float peak = 0;
        for (int i = 0; i < 48000*10; ++i) {
            const float x = i < 48000*2 ? .9f*rng.bipolar() : 0.f;
            const auto out = rig.step({x, x});
            CHECK(std::isfinite(out.l) && std::isfinite(out.r));
            peak = std::max({peak, std::abs(out.l), std::abs(out.r)});
        }
        std::printf("%s: full-bloom peak %.3f, loop energy %.3f\n", mode ? "Spectral" : "Cloud", peak, rig.dsp.loopEnergy());
        CHECK(peak <= .981f && rig.dsp.loopEnergy() < 2.f);
    }
}

// Below 100%, Bloom never sustains: a burst at any Space, Shimmer, and character dies away.
// Tested at 85% to leave margin for modes the measured worst case did not land on.
// (This caught the pure-tone Spectral path ringing a reverb mode into self-oscillation.)
void tailsAlwaysDecay() {
    for (int mode : {0, 1}) for (double space : {55., 80., 95.}) for (double shimmer : {0., 60.}) {
        Rig rig;
        rig.set(Character, mode); rig.set(Mix, 100); rig.set(Bloom, 85); rig.set(Space, space);
        rig.set(Shimmer, shimmer); rig.set(Time, 120); rig.set(Voices, 0);
        std::vector<float> out(48000*26);
        for (size_t i = 0; i < out.size(); ++i) {
            const float x = i < 24000 ? static_cast<float>(.3*sineAt(220, i, 48000)+.2*sineAt(330, i, 48000)) : 0.f;
            out[i] = rig.step({x, x}).l;
        }
        double peak = 0;
        for (size_t s = 0; s < 26; ++s) peak = std::max(peak, rms(out, s*48000, (s+1)*48000));
        const double late = rms(out, 48000*21, 48000*26);
        std::printf("%s space %.0f shimmer %.0f, bloom 85: tail %.1f dB below peak after 21 s\n", mode ? "Spectral" : "Cloud", space, shimmer, -20*std::log10(late/peak+1.e-12));
        CHECK(late < peak*.03);
    }
}

// Randomized automation across everything, several sample rates: always finite and limited.
void fuzz() {
    for (double rate : {44100., 48000., 96000.}) {
        Rig rig(rate);
        Rng rng; rng.state = 0x1234abcd;
        float peak = 0;
        const size_t blocks = static_cast<size_t>(rate*12/128);
        for (size_t b = 0; b < blocks; ++b) {
            if (b % 8 == 0) {
                for (uint32_t id = 0; id < ParamCount; ++id) {
                    if (id == Bypass) continue;
                    const auto& d = parameters[id];
                    const bool jump = rng.next() < .3f;
                    if (!jump && id != Character && id != Freeze) continue;
                    rig.p[id] = sanitize(id, rng.next() < .2f ? (rng.next() < .5f ? d.min : d.max) : d.min+(d.max-d.min)*rng.next());
                    if (id == Freeze && rng.next() < .6f) rig.p[id] = 0;
                }
                rig.dsp.setParameters(rig.p);
                if (b % 64 == 0) rig.dsp.setTempo(40+rng.next()*200);
            }
            for (int i = 0; i < 128; ++i) {
                const double t = static_cast<double>(b*128+i)/rate;
                const float x = static_cast<float>(.5*std::sin(2*pi*(110+rng.next()*0)*t)*std::exp(-std::fmod(t, 1.5)*2)) + .3f*rng.bipolar();
                const auto out = rig.step({x, x*.8f});
                CHECK(std::isfinite(out.l) && std::isfinite(out.r));
                peak = std::max({peak, std::abs(out.l), std::abs(out.r)});
            }
        }
        std::printf("fuzz at %.0f Hz: peak %.3f\n", rate, peak);
        CHECK(peak <= .981f);
    }
}

void switchingCharacterIsContinuous() {
    Rig rig;
    rig.set(Mix, 100); rig.set(Bloom, 60); rig.set(Shimmer, 70);
    float previous = 0, worstJump = 0;
    for (int i = 0; i < 48000*6; ++i) {
        if (i % 24000 == 0) rig.set(Character, (i/24000) % 2);
        const float x = static_cast<float>(.3*sineAt(220, i, 48000));
        const float out = rig.step({x, x}).l;
        if (i > 20000) worstJump = std::max(worstJump, std::abs(out-previous));
        previous = out;
    }
    std::printf("character switching: worst sample-to-sample jump %.3f\n", worstJump);
    CHECK(worstJump < .35f);
}

void sampleRateLimits() {
    Processor dsp;
    bool threw = false;
    try { dsp.prepare(100); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    for (double rate : {22050., 192000.}) { Rig rig(rate); for (int i = 0; i < 4000; ++i) CHECK(std::isfinite(rig.step({.1f, .1f}).l)); }
}

int main() {
    dryAndBypassAreExact();
    echoLandsOnTime();
    shimmerCascades();
    freezeSustains();
    fullBloomStaysBounded();
    tailsAlwaysDecay();
    fuzz();
    switchingCharacterIsContinuous();
    sampleRateLimits();
    std::printf("Processor: exact dry/bypass, echo timing, shimmer cascade, freeze/release, bounded loop, fuzzing, and character switching passed\n");
}
