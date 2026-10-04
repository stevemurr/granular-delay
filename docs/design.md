# Design notes

## Signal flow

`Processor` runs one loop per sample:

1. `loopIn = input × inputGain + feedback` (input gain goes to 0 on Freeze).
2. Both engines record `loopIn`. Only the active one (or both, for the 40 ms Character crossfade) renders its voice stack.
3. The stack goes through the reverb (blended in by Space), a two-pole low-pass, and a 40 Hz high-pass that keeps pitched-down voices from piling up bass.
4. `feedback = softclip(wet × gain)`, where `gain` comes from Bloom (or the freeze hold), and `softclip(x) = 2·tanh(x/2)` is linear well below clipping.
5. Output is the dry/wet mix, output gain, a stereo-linked 0.98 peak ceiling, and the bypass crossfade.

Pitched voices are always power-normalized against each other, and Shimmer is an equal-power crossfade between the unpitched voice (voice 0) and the pitched ones, so the loop's power gain is about one per pass regardless of the preset.

## Cloud engine

A grain is a read head that moves behind the write head by `1 − speed` samples per sample, windowed with a Hann envelope and read with cubic Hermite interpolation. At Texture 0 each voice launches grains at exactly half their length with no jitter, so two Hann windows sum to one: the unpitched voice is an essentially exact delay (tested to 2e-4 relative error), and a pitched voice is the classic two-head shifter.

A grain must stay behind the write head for its whole life, so a grain at speed `s` starts at least `(s − 1) × length` samples back. An octave voice therefore has an effective lookback of at least one grain length (110 ms at Texture 0) however short **Time** is. This is inherent to causal pitch shifting.

## Spectral engine

2048-point STFT at 48 kHz (the size scales with sample rate to keep roughly 40 ms), hop N/4, Hann analysis and synthesis. L and R share one complex FFT. Output trails input by `N + hop − 1` samples (2559, about 53 ms, at 48 kHz); each frame's work is cut into six stages spread across a hop so no single audio block pays for a whole frame. `Processor` subtracts the latency from the delay so the loop stays on the requested time whenever **Time** is at least that long; below it, the loop is as short as the engine allows.

Pitch shifting follows Laroche and Dolson's rigid phase locking: every spectral peak moves to its new frequency with its neighbouring bins, keeping the partial's shape and relative phases. A *region* is a run of bins whose per-bin instantaneous-frequency estimates agree to within 0.6 bin, with the loudest bin as representative. Choosing regions by frequency agreement, not by magnitude peaks, is what lets partials closer together than a Hann lobe (about 94 Hz) stay independent: a 220/277/330 Hz chord shifts to 440/554/659 Hz with every partial at its own level. Voices are summed in the frequency domain, so there is one inverse transform however many voices play.

The unpitched voice with Warp at zero copies the analysis spectrum, including DC and Nyquist, straight to the output; the Spectral engine is then a transparent delay (tested to 1e-7 relative error).

Limits: transients smear across a 43 ms window, which is the character of a phase vocoder; and below about 100 Hz a Hann lobe spans more than a harmonic, so very low material is shifted more coarsely.

## Reverb

An eight-channel orthogonal feedback network with input all-pass diffusion, from the previous version, now with ±0.7 ms slow delay modulation on every tap. The modulation matters: without it a held tone that lands on a mode rings 15–19 dB above the rest of the spectrum, and in a loop with a pure-tone engine that is enough to self-oscillate. It runs fully wet inside the loop; Space blends it in. Noise passes through it about 6–14 dB quieter than it went in, so the wet *output* (never the loop) gets makeup gain that tracks the decay time.

## Stability

A feedback loop with a resonant reverb and a lossless pitch engine can ring. Three layers keep that under control:

1. **A Bloom ceiling.** The feedback is scaled by the inverse of the reverb's measured worst-case tonal gain, `0.71 × (decay / 0.5 s)^0.39`, so the loop gain at the worst mode is Bloom itself.
2. **A governor.** Every 250 ms, if the input is quiet and the loop power is falling slower than about 1.5 dB per second (a healthy tail at an 18 s decay falls at 3 dB/s), feedback is trimmed by 10%; it recovers slowly once the loop decays. It is disabled at Bloom 97% and above, where infinite sustain is the point. This exists because the ceiling alone missed a Spectral case at long decays with Shimmer at 0.
3. **Soft clipping** in the feedback path, so even Bloom 100% stays bounded.

`tailsAlwaysDecay` in `tests/ProcessorTests.cpp` sweeps character, Space and Shimmer at Bloom 85% and requires every tail to fall by 30 dB within 21 seconds.

## Parameters and state

IDs 0–16, in the order of `src/Parameters.h`; append, never reorder. State is a little-endian `SHMR` v1 record of (id, double) pairs. Sessions from 0.x (`GRDL`) are refused, so the host keeps the new defaults instead of misreading four lines of settings as macros.

## Cost

Measured on the development Mac (Release, 48 kHz): Cloud about 1–2% of one core, Spectral with a four-voice preset about 3%. At 96 kHz Spectral rises to about 6%; its worst 64-sample block is about 40% of that block's time budget.

## Verification

`ctest` runs three suites (also clean under `-DGRANULAR_SANITIZE=ON`, which takes a few minutes), and `clap-validator` passes with no failures.

- `engine_tests`: FFT round trip; exact Cloud delay; pitch accuracy of both engines (Spectral within 0.02% for shifts from 14 cents to two octaves); chord partials; each warp type; Freeze sustain; Texture smear; idle recording.
- `processor_tests`: exact dry and bypass; echo timing (Cloud and Spectral, synced and free); shimmer cascade; Freeze and release; full-Bloom boundedness; tails always decay; fuzzing at 44.1/48/96 kHz; Character switching; sample-rate limits.
- `plugin_tests`: CLAP parameters, text conversion, state round trips and corruption, refusal of old sessions, remote controls, sample-accurate automation, and host processing.

What these cannot establish is how it sounds. The audio is checked by measurement (spectra, levels, tails), and the demo WAVs are there to be listened to.
