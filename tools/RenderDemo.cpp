#include "Processor.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

using namespace granular;
constexpr double pi = 3.141592653589793;
constexpr double rate = 48000;

void little(std::ofstream& f, uint32_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) f.put(static_cast<char>(value >> (i*8)));
}
void wav(const std::filesystem::path& path, const std::vector<Stereo>& samples) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot create " + path.string());
    const auto bytes = static_cast<uint32_t>(samples.size()*4);
    f.write("RIFF",4); little(f, 36+bytes,4); f.write("WAVEfmt ",8);
    little(f,16,4); little(f,1,2); little(f,2,2); little(f,48000,4);
    little(f,192000,4); little(f,4,2); little(f,16,2); f.write("data",4); little(f,bytes,4);
    for (auto s : samples) for (float x : {s.l,s.r})
        little(f, static_cast<uint16_t>(static_cast<int16_t>(std::clamp(x,-1.f,1.f)*32767)), 2);
    if (!f) throw std::runtime_error("Failed writing " + path.string());
}
// A plucked-chord phrase: Am, F, C, G, one chord per two seconds, then silence for the tail.
std::vector<Stereo> phrase(double seconds, double playSeconds) {
    const double chords[4][3] {{220, 261.63, 329.63}, {174.61, 220, 261.63}, {261.63, 329.63, 392}, {196, 246.94, 293.66}};
    std::vector<Stereo> dry(static_cast<size_t>(seconds*rate));
    for (size_t i = 0; i < dry.size(); ++i) {
        const double t = static_cast<double>(i)/rate;
        if (t >= playSeconds) continue;
        const auto& chord = chords[static_cast<size_t>(t/2)%4];
        const double pluck = std::fmod(t, 2.0);
        double x = 0;
        for (double hz : chord) x += std::exp(-pluck*1.6)*(std::sin(2*pi*hz*t)+.35*std::sin(2*pi*2*hz*t)+.12*std::sin(2*pi*3*hz*t));
        dry[i] = {static_cast<float>(.12*x), static_cast<float>(.12*x)};
    }
    return dry;
}
std::vector<Stereo> render(const std::vector<Stereo>& dry, Values p, double freezeAt = -1, double releaseAt = -1) {
    Processor dsp; dsp.setParameters(p); dsp.prepare(rate); dsp.setTempo(100);
    std::vector<Stereo> out(dry.size());
    for (size_t i = 0; i < dry.size(); ++i) {
        const double t = static_cast<double>(i)/rate;
        if (freezeAt >= 0 && i == static_cast<size_t>(freezeAt*rate)) { p[Freeze] = 1; dsp.setParameters(p); }
        if (releaseAt >= 0 && i == static_cast<size_t>(releaseAt*rate)) { p[Freeze] = 0; dsp.setParameters(p); }
        (void)t;
        out[i] = dsp.process(dry[i]);
    }
    return out;
}
int main(int argc, char** argv) {
    try {
        std::filesystem::path folder = argc > 1 ? argv[1] : "demos";
        std::filesystem::create_directories(folder);
        const auto dry = phrase(16, 8);
        wav(folder/"00-dry.wav", dry);

        auto p = defaults();
        // 1. The default shimmer: octave up, lots of space.
        p[Mix] = 55; p[Shimmer] = 60; p[Bloom] = 62; p[Space] = 70; p[Texture] = 15; p[Time] = 90;
        wav(folder/"01-shimmer-octave.wav", render(dry, p));
        // 2. A wider stack, a darker tone, a longer bloom.
        p[Voices] = 2; p[Bloom] = 72; p[Tone] = 4500; p[Space] = 80;
        wav(folder/"02-shimmer-octave-fifth-dark.wav", render(dry, p));
        // 3. Cloud, scattered: texture and warp up.
        p = defaults(); p[Mix] = 60; p[Shimmer] = 70; p[Voices] = 4; p[Texture] = 70; p[Warp] = 45; p[Bloom] = 60; p[Space] = 60; p[Time] = 250;
        wav(folder/"03-cloud-scattered.wav", render(dry, p));
        // 4. Spectral, stretch: metallic, bell-like partials.
        p = defaults(); p[Character] = 1; p[Mix] = 60; p[Shimmer] = 80; p[Voices] = 5; p[WarpType] = 0; p[Warp] = 70; p[Texture] = 40; p[Bloom] = 62; p[Space] = 65; p[Time] = 120;
        wav(folder/"04-spectral-stretch.wav", render(dry, p));
        // 5. Spectral, shift: inharmonic sidebands drifting through the loop.
        p[WarpType] = 1; p[Warp] = 40; p[Voices] = 10;
        wav(folder/"05-spectral-shift.wav", render(dry, p));
        // 6. Spectral, whisper + smear: the chords dissolve into breath.
        p[WarpType] = 3; p[Warp] = 65; p[Texture] = 85; p[Voices] = 0; p[Interval] = 100;
        wav(folder/"06-spectral-whisper-smear.wav", render(dry, p));
        // 7. Frozen shimmer: capture at 5 s, release at 12 s; the source stops at 8 s.
        p = defaults(); p[Mix] = 70; p[Shimmer] = 65; p[Voices] = 5; p[Bloom] = 50; p[Space] = 75; p[Texture] = 25;
        wav(folder/"07-cloud-freeze.wav", render(dry, p, 5, 12));
        p[Character] = 1; p[WarpType] = 2; p[Warp] = 25;
        wav(folder/"08-spectral-freeze.wav", render(dry, p, 5, 12));
        std::cout << "Rendered nine examples in " << folder << " (source stops at 8 s; freeze 5-12 s in the last two)\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
