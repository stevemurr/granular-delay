# Controls

Seventeen parameters, eight of them knobs. Everything automates, and everything except the switches and pickers accepts CLAP modulation. Bitwig has two remote-control pages: **Shimmer** (Shimmer, Bloom, Space, Time, Texture, Tone, Mix, Freeze) and **Voices & Warp** (Character, Voices, Interval, Warp, Warp Type, Sync, Division, Output).

## Where the time goes

You will spend nearly all of it on these:

| Control | Range | What it does |
| --- | --- | --- |
| **Shimmer** | 0–100% | Balance between the unpitched signal and the pitched voice stack inside the loop (equal-power). 0% is a plain delay-reverb; 100% is only pitched voices. |
| **Bloom** | 0–100% | Feedback: how much of the loop comes around again. Below 100% every tail dies away; 100% is the edge of sustain. See [design](design.md#stability). |
| **Space** | 0–100% | Reverb inside the loop. The reverb fades in over the first 60% of the knob; decay runs from 0.3 s to 18 s across the whole range. |
| **Time** | 20–2000 ms | Loop delay. With **Sync** on, the knob selects a note division instead (nine, from 1/8 to 2 bars; the delay is capped at 5 s). |
| **Tone** | 0.8–18 kHz | Low-pass inside the loop (two poles) plus reverb damping. Darker tone makes a shimmer settle instead of piling up on top. |
| **Mix / Output** | 0–100% / −24…+12 dB | Dry/wet and final level. Use 100% on a send. |

## Voices and Interval

**Voices** picks an interval stack; the plugin builds and mixes the pitched voices itself.

| Preset | Semitones |
| --- | --- |
| Octave | +12 |
| Fifth | +7 |
| Octave + Fifth | +12, +19 |
| Two Octaves | +12, +24 |
| Up & Down | +12, −12 |
| Major | +12, +16, +19 |
| Minor | +12, +15, +19 |
| Sus | +12, +17, +19 |
| Sub | −12 |
| Deep | −12, −24 |
| Stack | +7, +12, +19, +24 |
| Detune | +14 cents, −14 cents |
| Cluster | −5, +1, +6, +13 |

**Interval** (the pad's X axis) scales the whole stack: 100% is as written, 0% collapses it to unison, 200% doubles every interval. The pad snaps to exactly 100% within ±3% (hold Shift to disable). Intermediate values detune the stack into intervals that are not on any scale, which is where Spectral gets strange.

## Warp and Texture

Warp is the pad's Y axis. What it does depends on **Character**.

**Cloud.** Warp is *wildness*, and it grows quickly past the first third of the knob so the low end stays musical:
- pitch scatter of up to ±5 semitones per grain (grows with the square of Warp),
- random octave jumps (up to 50% of grains),
- per-grain pitch bends of up to ±14 semitones (grows with the cube),
- reversed grains (up to 50%).

**Texture** (Cloud) runs from the classic smooth two-head shifter to a scattered cloud: grains shrink from 110 ms to about 60 ms, overlap rises from 2 to 7, and spray (up to 350 ms of random offset) and stereo scatter grow.

**Spectral.** Pick a **Warp Type**; Warp is its amount:

| Type | What it does |
| --- | --- |
| Stretch | Inharmonic, like stiff piano strings: low partials barely move, upper ones fly out. Metallic and bell-like. |
| Shift | A constant-Hz offset (up to 2 kHz) that alternates direction per voice. Harmonics stop being harmonic. |
| Scatter | Every partial wanders on its own slow random walk of up to ±900 Hz. |
| Whisper | Random phase on every bin: a tone dissolves into breath. |
| Crystal | Keeps the partials and thins everything between them; quiet partials go first. |

**Texture** (Spectral) smears the spectrum in time: peaks are held with a fast attack and slow release (up to about a second) and blurred across neighbouring bins, so notes bloom into pads and sustain after the source stops.

## Freeze

Freeze mutes the input and holds the loop at the level it had at the instant of freezing. A slow gain control compensates for what the tone filter and reverb remove, so the held sound neither fades nor runs away. In Cloud it keeps cascading through the voice stack; in Spectral the analysis frame is held too, so the held spectrum resynthesizes as a steady chord through the same voices. Releasing returns to live audio with the held sound decaying naturally.

## Switches

- **Character** crossfades between Cloud and Spectral over 40 ms. Both engines keep recording the loop, so switching needs no warm-up beyond the Spectral engine's latency.
- **Bypass** is a smoothed dry bypass.
- Processed output has a stereo-linked 0.98 sample-peak ceiling (instant attack, smooth recovery). It is not an oversampled true-peak limiter.

## Starting points

- **Classic shimmer:** defaults (Cloud, Octave, Shimmer 55%, Bloom 55%, Space 70%). Play pads or long notes; raise Mix to taste.
- **Dark cathedral:** Voices *Octave + Fifth*, Shimmer 70%, Bloom 72%, Space 85%, Tone 4.5 kHz.
- **Grain fog:** Cloud, Voices *Major*, Texture 70%, Warp 45%, Time 250 ms.
- **Bell tower:** Spectral, Voices *Sus*, Warp Type *Stretch*, Warp 70%, Texture 40%.
- **Sideband drift:** Spectral, Voices *Stack*, Warp Type *Shift*, Warp 40%, Bloom 62%.
- **Dissolve:** Spectral, Warp Type *Whisper*, Warp 65%, Texture 85%.
- **Held chord:** play a chord, hit Freeze, then sweep Interval and Warp over the held sound.

`build/render_demo artifacts/demos` renders nine phrases covering these.
