#include "CloudEngine.h"
#include "Fft.h"
#include "SpectralEngine.h"
#include "Test.h"
#include <algorithm>
#include <cmath>
#include <vector>

using namespace granular;
constexpr double pi = 3.14159265358979323846;
constexpr double rate = 48000;

struct Spectrum {
    std::vector<float> mag;
    double binHz = 0;
    double peakHz(double lo = 20, double hi = 20000) const {
        size_t best = 0;
        for (size_t k = static_cast<size_t>(lo/binHz)+1; k < std::min(mag.size()-1, static_cast<size_t>(hi/binHz)); ++k)
            if (mag[k] > mag[best]) best = k;
        const double a = mag[best-1], b = mag[best], c = mag[best+1];
        const double denom = a-2*b+c;
        const double shift = denom != 0 ? .5*(a-c)/denom : 0;
        return (static_cast<double>(best)+shift)*binHz;
    }
    double level(double hz, double width = 30) const { // peak magnitude near hz
        float m = 0;
        for (size_t k = 1; k+1 < mag.size(); ++k)
            if (std::abs(static_cast<double>(k)*binHz-hz) < width) m = std::max(m, mag[k]);
        return m;
    }
};
// Hann-windowed spectrum of the last 16384 samples of x.
Spectrum spectrum(const std::vector<float>& x, size_t n = 16384) {
    Fft fft; fft.prepare(n);
    std::vector<Cpx> work(n);
    const size_t start = x.size()-n;
    for (size_t i = 0; i < n; ++i)
        work[i] = {x[start+i]*static_cast<float>(.5-.5*std::cos(2*pi*static_cast<double>(i)/static_cast<double>(n))), 0};
    fft.forward(work.data());
    Spectrum s; s.binHz = rate/static_cast<double>(n);
    for (size_t k = 0; k <= n/2; ++k) s.mag.push_back(std::sqrt(work[k].re*work[k].re+work[k].im*work[k].im)*4.f/static_cast<float>(n));
    return s;
}
double rms(const std::vector<float>& x, size_t from, size_t to) {
    double sum = 0;
    for (size_t i = from; i < to; ++i) sum += static_cast<double>(x[i])*x[i];
    return std::sqrt(sum/static_cast<double>(to-from));
}
VoiceSet voices(std::initializer_list<std::pair<float,float>> pitched, float dry) {
    VoiceSet v; v.gain[0] = dry;
    size_t i = 1;
    for (auto [st, g] : pitched) { v.semitones[i] = st; v.gain[i] = g; ++i; }
    return v;
}
std::vector<float> sine(double hz, size_t n, double amp = .5) {
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i) x[i] = static_cast<float>(amp*std::sin(2*pi*hz*static_cast<double>(i)/rate));
    return x;
}
std::vector<float> noise(size_t n, double amp = .3) {
    Rng rng; std::vector<float> x(n);
    for (auto& v : x) v = static_cast<float>(amp)*rng.bipolar();
    return x;
}
double db(double ratio) { return 20*std::log10(ratio); }

void fftRoundTrip() {
    Fft fft; fft.prepare(1024);
    std::vector<Cpx> x(1024), original;
    Rng rng;
    for (auto& v : x) v = {rng.bipolar(), rng.bipolar()};
    original = x;
    fft.forward(x.data()); fft.inverse(x.data());
    for (size_t i = 0; i < x.size(); ++i) {
        CHECK(std::abs(x[i].re/1024-original[i].re) < 1.e-5f);
        CHECK(std::abs(x[i].im/1024-original[i].im) < 1.e-5f);
    }
    const auto s = spectrum(sine(1000, 16384, 1.0));
    CHECK(std::abs(s.peakHz()-1000) < 1);
    CHECK(std::abs(s.level(1000)-1.0) < .08);
}

// Cloud, Texture 0, unpitched voice only: a plain delay of exactly the requested time.
void cloudDelayIsExact() {
    CloudEngine cloud; cloud.prepare(rate);
    cloud.setDelay(2400); cloud.setTexture(0); cloud.setWarp(0);
    const auto in = noise(48000);
    const auto v = voices({}, 1.f);
    double err = 0;
    for (size_t i = 0; i < in.size(); ++i) {
        const auto out = cloud.process({in[i], in[i]}, v, true);
        if (i > 12000) err += std::pow(out.l-in[i-2400], 2);
    }
    const double relative = std::sqrt(err/static_cast<double>(in.size()-12000))/.173; // noise rms = .3/sqrt(3)
    std::printf("cloud exact-delay relative error: %.2e\n", relative);
    CHECK(relative < 2.e-3);
}

void cloudShiftsPitch() {
    for (float st : {12.f, 7.f, -12.f, 24.f}) {
        CloudEngine cloud; cloud.prepare(rate);
        cloud.setDelay(2400); cloud.setTexture(0); cloud.setWarp(0);
        const auto in = sine(440, 96000);
        const auto v = voices({{st, 1.f}}, 0.f);
        std::vector<float> out(in.size());
        for (size_t i = 0; i < in.size(); ++i) out[i] = cloud.process({in[i], in[i]}, v, true).l;
        const auto s = spectrum(out);
        const double expected = 440*std::pow(2., st/12.);
        const double got = s.peakHz(50, 12000);
        const double level = s.level(expected, 25);
        std::printf("cloud %+.0f st: peak %.1f Hz (want %.1f), level %.2f dB re 0.5\n", st, got, expected, db(level/.5));
        CHECK(std::abs(got-expected) < expected*.01);
        CHECK(std::abs(db(level/.5)) < 5.); // two-head crossfades spread a line into sidebands
    }
}

void cloudTextureAndWarpStayBounded() {
    CloudEngine cloud; cloud.prepare(rate);
    cloud.setDelay(100); cloud.setTexture(1); cloud.setWarp(1);
    VoiceSet v = voices({{24.f, 1.f}, {-24.f, 1.f}, {12.f, 1.f}, {7.f, 1.f}}, 1.f);
    const auto in = noise(96000, .5);
    float peak = 0;
    for (size_t i = 0; i < in.size(); ++i) {
        const auto out = cloud.process({in[i], in[i]}, v, true);
        CHECK(std::isfinite(out.l) && std::isfinite(out.r));
        peak = std::max({peak, std::abs(out.l), std::abs(out.r)});
    }
    std::printf("cloud worst-case peak with five voices: %.2f\n", peak);
    CHECK(peak < 12.f);
}

struct SpectralRun {
    std::vector<float> out;
};
SpectralRun runSpectral(const std::vector<float>& in, const VoiceSet& v, double delay, int type = 0, double warp = 0, double texture = 0) {
    SpectralEngine spectral; spectral.prepare(rate);
    spectral.setDelay(delay); spectral.setWarp(type, warp); spectral.setTexture(texture);
    SpectralRun r; r.out.resize(in.size());
    for (size_t i = 0; i < in.size(); ++i) r.out[i] = spectral.process({in[i], in[i]}, v, true).l;
    return r;
}

void spectralIdentity() {
    SpectralEngine spectral; spectral.prepare(rate);
    const double delay = 1000;
    spectral.setDelay(delay); spectral.setWarp(0, 0); spectral.setTexture(0);
    const size_t latency = spectral.latency();
    const auto in = noise(48000);
    const auto v = voices({}, 1.f);
    double err = 0;
    for (size_t i = 0; i < in.size(); ++i) {
        const auto out = spectral.process({in[i], in[i]}, v, true);
        const size_t lag = latency+static_cast<size_t>(delay);
        if (i > lag+4096) err += std::pow(out.l-in[i-lag], 2);
    }
    const double relative = std::sqrt(err/static_cast<double>(in.size()-4096-latency-1000))/.173;
    std::printf("spectral identity (latency %zu): relative error %.2e\n", latency, relative);
    CHECK(relative < 1.e-2);
}

void spectralShiftsPitch() {
    for (float st : {12.f, 7.f, -12.f, 24.f, .14f}) {
        const auto in = sine(440, 96000);
        const auto v = voices({{st, 1.f}}, 0.f);
        const auto r = runSpectral(in, v, 1000);
        const auto s = spectrum(r.out);
        const double expected = 440*std::pow(2., st/12.);
        const double got = s.peakHz(50, 12000);
        const double level = s.level(expected, 25);
        std::printf("spectral %+.2f st: peak %.1f Hz (want %.1f), level %.2f dB re 0.5\n", st, got, expected, db(level/.5));
        CHECK(std::abs(got-expected) < expected*.01+1);
        CHECK(std::abs(db(level/.5)) < 3.);
    }
}

void spectralChordKeepsEveryPartial() {
    std::vector<float> in(96000, 0.f);
    for (double hz : {220., 277.18, 329.63}) {
        const auto s = sine(hz, in.size(), .2);
        for (size_t i = 0; i < in.size(); ++i) in[i] += s[i];
    }
    const auto v = voices({{12.f, 1.f}}, 0.f);
    const auto s = spectrum(runSpectral(in, v, 1000).out);
    for (double hz : {440., 554.36, 659.26}) {
        const double level = s.level(hz, 12);
        std::printf("spectral chord partial %.1f: %.3f (want ~0.2)\n", hz, level);
        CHECK(level > .14 && level < .28);
    }
    // And nothing meaningful left at the unshifted partials.
    CHECK(s.level(220, 8) < .02);
}

void spectralWarpTypes() {
    const auto in = sine(440, 96000, .4);
    // Stretch: partials rise, more so the higher they sit (a 440 Hz tone barely; its overtones a lot).
    {
        const auto s = spectrum(runSpectral(in, voices({}, 1.f), 1000, 0, 1.0).out);
        const double hz = s.peakHz(50, 12000);
        std::printf("stretch w=1 moves 440 Hz to %.1f Hz\n", hz);
        CHECK(hz > 450 && hz < 520);
        std::vector<float> rich(96000);
        for (size_t i = 0; i < rich.size(); ++i) { const double t = static_cast<double>(i)/rate; rich[i] = static_cast<float>(.2*std::sin(2*pi*440*t)+.2*std::sin(2*pi*1320*t)+.2*std::sin(2*pi*2200*t)); }
        const auto r = spectrum(runSpectral(rich, voices({}, 1.f), 1000, 0, 1.0).out);
        const double top = r.peakHz(1800, 12000);
        std::printf("stretch w=1 moves the 2200 Hz partial to %.1f Hz\n", top);
        CHECK(top > 3500);
    }
    // Shift: constant-Hz offset moves the partial by up to ~2 kHz on the dry voice.
    {
        const auto s = spectrum(runSpectral(in, voices({}, 1.f), 1000, 1, 1.0).out);
        const double hz = s.peakHz(50, 12000);
        std::printf("shift w=1 moves 440 Hz to %.1f Hz\n", hz);
        CHECK(std::abs(hz-2440) < 80);
    }
    // Whisper at full: a clean tone dissolves into noise (peak no longer dominates).
    {
        const auto clean = spectrum(runSpectral(in, voices({}, 1.f), 1000, 3, 0.0).out);
        const auto whisper = spectrum(runSpectral(in, voices({}, 1.f), 1000, 3, 1.0).out);
        float cleanSum = 0, whisperSum = 0;
        for (size_t k = 0; k < clean.mag.size(); ++k) { cleanSum += clean.mag[k]; whisperSum += whisper.mag[k]; }
        const double cleanPeak = clean.level(440, 30)/cleanSum, whisperPeak = whisper.level(440, 30)/whisperSum;
        std::printf("whisper: peak share %.3f -> %.3f\n", cleanPeak, whisperPeak);
        CHECK(whisperPeak < cleanPeak*.5);
    }
    // Every type, at full, with several voices and noise: finite and bounded.
    for (int type = 0; type < 5; ++type) {
        const auto r = runSpectral(noise(96000, .5), voices({{12.f, 1.f}, {-12.f, 1.f}, {7.f, 1.f}, {19.f, 1.f}}, 1.f), 500, type, 1.0, 1.0);
        float peak = 0;
        for (auto x : r.out) { CHECK(std::isfinite(x)); peak = std::max(peak, std::abs(x)); }
        std::printf("warp type %d, full: peak %.2f\n", type, peak);
        CHECK(peak < 12.f);
    }
}

void spectralFreezeSustains() {
    SpectralEngine spectral; spectral.prepare(rate);
    spectral.setDelay(1000); spectral.setWarp(0, 0); spectral.setTexture(0);
    const auto v = voices({{12.f, .8f}}, .6f);
    const auto tone = sine(330, 24000, .4);
    std::vector<float> out;
    for (size_t i = 0; i < 96000; ++i) {
        if (i == 24000) spectral.setFreeze(true);
        const float x = i < tone.size() ? tone[i] : 0.f; // source stops exactly at the freeze
        out.push_back(spectral.process({x, x}, v, true).l);
    }
    const double early = rms(out, 30000, 36000), late = rms(out, 90000, 96000);
    std::printf("spectral freeze: rms %.3f -> %.3f after the source stops\n", early, late);
    CHECK(early > .05);
    CHECK(std::abs(db(late/early)) < 1.5);
    const auto s = spectrum(out);
    CHECK(std::abs(s.peakHz(50, 2000)-330) < 3 || std::abs(s.peakHz(50, 2000)-660) < 6);
}

void spectralTextureSustainsAfterSourceStops() {
    const auto tone = sine(330, 24000, .4);
    std::vector<float> in(96000, 0.f);
    std::copy(tone.begin(), tone.end(), in.begin());
    const auto v = voices({}, 1.f);
    const auto crisp = runSpectral(in, v, 1000, 0, 0, 0.0).out;
    const auto smeared = runSpectral(in, v, 1000, 0, 0, 1.0).out;
    // Compare the energy that remains 0.5 s after the source ends (plus engine latency).
    const double a = rms(crisp, 48000+3000, 48000+9000), b = rms(smeared, 48000+3000, 48000+9000);
    std::printf("texture: tail rms %.4f (crisp) vs %.4f (full smear)\n", a, b);
    CHECK(b > a*10 + 1.e-4);
}

void inactiveEnginesOnlyRecord() {
    SpectralEngine spectral; spectral.prepare(rate);
    CloudEngine cloud; cloud.prepare(rate);
    const auto v = voices({{12.f, 1.f}}, 1.f);
    spectral.setDelay(500); cloud.setDelay(500);
    for (size_t i = 0; i < 20000; ++i) {
        const float x = static_cast<float>(.5*std::sin(i*.05));
        const auto a = spectral.process({x, x}, v, false);
        const auto b = cloud.process({x, x}, v, false);
        CHECK(a.l == 0 && a.r == 0 && b.l == 0 && b.r == 0);
    }
    // Switching on later needs no warm-up beyond the engine's own latency.
    float peak = 0;
    for (size_t i = 0; i < 8000; ++i) {
        const float x = static_cast<float>(.5*std::sin(i*.05));
        const auto a = cloud.process({x, x}, v, true);
        peak = std::max(peak, std::abs(a.l));
    }
    CHECK(peak > .1f);
}

int main() {
    fftRoundTrip();
    cloudDelayIsExact();
    cloudShiftsPitch();
    cloudTextureAndWarpStayBounded();
    spectralIdentity();
    spectralShiftsPitch();
    spectralChordKeepsEveryPartial();
    spectralWarpTypes();
    spectralFreezeSustains();
    spectralTextureSustainsAfterSourceStops();
    inactiveEnginesOnlyRecord();
    std::printf("Engines: FFT, exact cloud delay, cloud/spectral pitch accuracy, chords, warp types, freeze, smear, and idle recording passed\n");
}
