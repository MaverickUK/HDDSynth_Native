# Drive Lab v2 (work in progress)

A rebuild of the modelled drive, aimed at sounding like the real recordings in
[HDDSynth_Samples](https://github.com/MaverickUK/HDDSynth_Samples). It will
replace `tools/drive-lab` (and the firmware's `src/drive_engine.*` port) once
it's ready. Until then v1 stays as it is.

**A drive is always pure synthesis at heart.** You can design one from
scratch (`drives/designed_7200.json`) or fit one from a recorded pack as a
starting point, then edit it. Recorded seek sounds will be an optional extra
per drive, never required.

## What the recordings showed

Measured over all 9 packs (16 kHz) against v1's presets:

- **The idle is a fingerprint of tones locked to the rotation rate.** For
  example, the Raptor's are at 1, 4, 13, 16 and 24 × 166.7 Hz (10,000 rpm).
  There are 10-14 strong tones per drive, up to 8 kHz and 30-50 dB above the
  noise. v1 had 3-5 and was much duller (idle centroid 0.4-1.3 kHz against
  1.2-4.9 kHz).
- **Spin-up and spindown sweep every tone together**, and each lights up as
  it passes the drive's fixed body resonances.
- **Seeks are 4-40 ms bursts, mostly a low "rock"**: one resonance per drive,
  steady from seek to seek (311 Hz on the Fireball EX, ~400 Hz on the Raptor).
  Some drives ring higher instead: 1.3 and 2.4 kHz on the WD Caviar, 1.1-3 kHz on
  Mark_Original. They come 9-40 a second, often back to back, and longer seeks
  are louder. v1's were sparse and clicky.
- **The stepper drive (MicroScience) hums at its step rate**, 240 Hz, whatever
  the seek's length.
- **Mark_IBM's access is sequential reading:** a 4 ms track-to-track step every
  two revolutions (16.5 ms).
- **Real tones flicker:** each one's level wanders by 1.5-3.5 dB over 50 ms
  frames (8 on the Ultrastar). Steady synthesised tones are part of what
  sounds artificial.
- **The packs' access.wav is quiet between seeks** (10-20 dB under the idle):
  the player mixes it over the idle loop, so that mix is what a seek is fitted
  to and compared with.

## The spindle model (`engine.js`)

- **Tones:** `order` (a multiple of the rotation rate; may be fractional) and
  `db` (its level at full speed). They scale with speed^`speedExp`.
  A `motor` tone instead follows the drive current: `motorBoostDb` louder
  while spinning up, and silent once the power is off.
- **Body:** up to 8 resonances (`hz`, `q`, `db`) that every tone and noise
  band passes through: the intuitive control for designing. Optionally also
  a `bodyCurve` (gain points `hz`, `db`, straight lines between them on a log
  axis): what fitting produces.
- **Noise:** air noise in 18 half-octave bands from 40 Hz (`db` is each
  band's RMS at full speed). Louder with speed^`speedExp`, and its bands
  shift with speed^`follow`.
- **Spindle:** `rpm`, `spinupS`, `coastS`, `friction`, `jitter` (slow speed
  wander) and `wobble` (AM at the rotation rate). `friction` (0-1) shapes the
  coast-down: it's constant friction's share of the braking at full speed,
  against windage. Near 0 the speed decays exponentially; at 1 it falls in a
  straight line. The default of 0.082 matches v1. The fitted drives range
  from 0.15 to 1. `flutterDb` makes each tone's level wander slowly and
  independently (its standard deviation, in dB), at about `flutterHz`
  (default 2). Faster flutter on a loud tone spills into the quiet bands
  beside it.
- **Bed** (optional, for designed drives): `count` faint tones around `db`, up
  to `maxOrder`, `fractional` of them off the whole orders, placed by `seed`
  (the same seed always gives the same bed). A real drive has dozens of faint
  tones besides its main ones; fitted drives get theirs from the recording.

## The actuator and the workload

- **Actuator:** `kind` is `voice` (voice coil) or `stepper`.
  - A voice-coil seek of distance d (a fraction of the stroke) takes
    `trackMs` + (`fullStrokeMs` - `trackMs`) * sqrt(d). The arm is pushed one
    way for the first half and braked for the second.
  - A stepper takes one step per 1/`stepHz`, `strokeSteps` for the full stroke.
  - The excitation is a mix: `clickDb` (the force switching on, reversing, off;
    for a stepper, each step), `thumpDb` (the force itself, softened) and
    `buzzDb` (the coil's current ripple, as noise while the force is on).
    `settleMs` is the tail after the head arrives. `levelExp` sets how much
    harder longer seeks push.
  - It's all filtered by `modes` (up to 8: `hz`, `q`, `db`), the arm and its
    mount, and scaled by `db`.
- **Workload** (while busy): after each seek, wait for the sector to come
  round (0 to 1 revolution) and then for the transfer, `gapMs` on average.
  - Random seeks go anywhere, except a share (`locality`) that stay close.
  - `seqShare` is the share of the time spent in sequential runs, with 1 for
    all of it. A sequential run steps one track every `seqRevs` revolutions.
  - The engine's `busy` message takes a class: `random` or `sequential` (as
    the activity link reports). A `seek` message moves the head to a position,
    for HDDSynthPro's head targets.

### Recorded seeks (grains), optional

`node grains.js PACK_DIR drive.json [--max N] [--mix M]` cuts up to N (24)
seeks from the pack's access.wav into `<drive>.grains.wav`, beside the drive.
It lists them in the drive's `grains`: `{ file, mix, items: [{ start, len, ms }] }`.

While seeking, the engine plays a grain at the seek's rank among random seeks,
so short seeks get short grains, as often as the recording had them. `mix`
crossfades the synthesised seek (0) and the recording (1), at equal power. The
head waits for the grain to finish.

Without `grains`, or with mix 0, the drive is pure synthesis. Grains are about
13-27 KB per drive at 16-bit.

| pack | seek spectrum: synthesis | mix 0.5 | grains only |
|---|---|---|---|
| PB_WD_Raptor | 4.5 dB | 2.9 dB | 1.8 dB |
| PB_QuantumFireballEX | 3.9 dB | 3.5 dB | 2.8 dB |
| PB_MicroScience612 | 4.4 dB | 3.0 dB | 1.4 dB |

(With grains only, the MicroScience's detected seek rate drops to 5/s against
the recording's 12/s: its many local seeks all pick the shortest, quietest
grains.)

## Tools (Node, no dependencies)

```sh
node fit.js PACK_DIR [--rpm N] [-o drive.json]   # fit a drive from a recorded pack
node render.js drive.json OUT_DIR [rate]          # render spinup/idle/access/spindown WAVs
node score.js SAMPLES_DIR [OUT_DIR] [--pack NAME] # fit, render and score every pack;
                                                  # OUT_DIR gets A/B WAVs to listen to
node grains.js PACK_DIR drive.json [--max N]      # optional: add recorded seeks to a drive
```

`dsp.js` (FFT, spectra, peaks, bands) has no Node dependencies, so the Lab
page can fit drives in the browser too (not yet).

### The Lab page

```sh
python3 build.py              # -> drive-lab.html (the page body, as v1's)
python3 build.py --site DIR   # -> DIR/index.html, a full page to open in a browser
```

The page lets you:
- play a drive live (AudioWorklet): power on and off, busy (random or
  sequential), and click the head bar to seek there
- edit every parameter, including tables for the tones, body and arm modes
- load the drive's grains WAV
- compare against a recording, with a live spectrum (the tones' current
  places marked) and a spectrogram
- open and save drive.json

The presets are the designed and fitted drives, embedded by `build.py`. It
isn't on GitHub Pages yet; v1 stays there until v2 replaces it.

How fitting works:
1. **Idle** (`fit.js`): the rotation rate, the 40 strongest tones as orders
   of it, and the noise bands. The noise is calibrated by rendering it and
   correcting, because the engine's bands overlap.
2. **Spin-up** (`fit.js`): the rotation rate is tracked through it (each
   frame scored against the drive's comb of whole-order tones, then a
   Viterbi pass that only lets the speed rise). The spin-up law is
   least-squares fitted to the part of the track it trusts.
3. **Spindown** (`spindown_fit.js`): the coast time and `friction`, how the
   tones' and noise's levels follow speed (`speedExp`), and the body curve.
   They're fitted together by predicting the spindown's band-by-time
   spectrogram from the drive's parameters, so no audio is rendered. Where
   the speed tracks cleanly, the trusted part of the track is an extra term
   in the error. The idle levels stay fixed, so the body only reshapes the
   spindown.
4. **Flutter** (`fit.js`): how much the strongest tones wander, less what the
   measure finds in a steady render, then set by rendering and correcting.
5. **Seeks** (`seek_fit.js`). `measure()` finds seeks in a recording against its
   idle: bursts in three bands (120-800 Hz, 0.8-2.5 kHz, 2.5-7 kHz) above the
   idle's 99th percentile by 6 dB. From them it takes:
   - the rate, the durations and the gaps, and how busy the drive is
   - what the seeks add to the spectrum
   - how their level grows with length, and how spiky they are

   Fitting works in two steps:
   - **A starting point.** The arm's modes are the peaks of the added spectrum.
     A step rate shows up as a low hum that stays within 6% from seek to seek.
   - **Six rounds of correction.** Render the seeks, add them to the recorded
     idle and measure that the same way. Then correct the mode levels, the seek
     times, `gapMs` (from how busy it is), `levelExp` and `clickDb`. A very
     regular rhythm becomes sequential reading.

## Where it stands

`score.js` against the samples:

| pack | rpm | idle level | idle bands | tones matched | spindown (flat body) | spindown (fitted) | seeks/s real / synth | busy real / synth | seek level | seek spectrum |
|---|---|---|---|---|---|---|---|---|---|---|
| Mark_IBM | 7267 | -1.2 dB | 1.9 dB | 100% | 10.0 dB | 5.7 dB | 64.5 / 55.6 | 33% / 36% | +0.8 dB | 7.4 dB |
| Mark_Original | 7205 | -0.4 dB | 1.3 dB | 100% | 12.1 dB | 7.2 dB | 31.5 / 27.7 | 28% / 34% | 0.0 dB | 5.6 dB |
| Mark_Toaster | 7208 | -1.2 dB | 2.0 dB | 93% | 5.4 dB | 5.4 dB | 12.7 / 11.5 | 46% / 42% | -0.8 dB | 3.6 dB |
| PB_IBM_Ultrastar | 10003 | -1.8 dB | 3.4 dB | 76% | 10.5 dB | 5.3 dB | 8.6 / 5.8 | 70% / 74% | +0.5 dB | 4.7 dB |
| PB_MicroScience612 | 3552 | -1.8 dB | 3.7 dB | 90% | 19.2 dB | 5.6 dB | 12.1 / 14.0 | 28% / 37% | +1.1 dB | 4.5 dB |
| PB_QuantumFireballEX | 5359 | -1.0 dB | 2.3 dB | 82% | 12.5 dB | 6.1 dB | 38.6 / 24.0 | 23% / 15% | +1.6 dB | 3.9 dB |
| PB_QuantumFireballTM | 4500 | -0.3 dB | 2.7 dB | 100% | 8.6 dB | 5.0 dB | 9.1 / 10.1 | 6% / 8% | +1.2 dB | 6.0 dB |
| PB_WD_Caviar2850 | 4502 | -1.3 dB | 3.1 dB | 98% | 10.4 dB | 5.9 dB | 20.1 / 20.7 | 21% / 32% | +0.3 dB | 2.9 dB |
| PB_WD_Raptor | 10004 | -0.4 dB | 1.2 dB | 97% | 6.9 dB | 4.8 dB | 13.5 / 15.0 | 14% / 16% | +0.2 dB | 4.5 dB |

The columns:
- **Idle bands:** the mean difference across third-octave bands.
- **Tones matched:** dips where the flutter is strong (Ultrastar, Fireball EX).
  A 4 s window can't average out a tone wandering by 5 dB.
- **Spindown:** the same measure as idle bands, frame by frame (0.25 s) over
  the recording, with both sides floored at the recording's background noise.
  "Flat body" is `score.js --no-spindown-fit`.
- **Seek columns:** `measure()` run over each side's own idle. The real side is
  access.wav over the idle loop; the synthesis is rendered while busy. The
  spectrum column is the third-octave difference of what the seeks add, floored
  30 dB below the recording's strongest band.

Known gaps:
- **Spindowns are about right in level and colour, but the falling tones
  don't flare as brightly as in the recordings** (clearest in
  `PB_QuantumFireballTM`, whose tones light up crossing 300-700 Hz). The body
  curve is limited to +-15 dB and smoothed, and third-octave bands reward the
  overall level more than the lines. 1/12-octave bands were tried and made
  it worse: the search dodged mismatched lines with too-short coasts.
- **Coast times are uncertain where the speed doesn't track**, and some
  recordings may brake actively (the Raptor fits a 2 s coast at friction 0.8).
- **`PB_IBM_Ultrastar`'s spindown has a steep line (1 kHz to 150 Hz in about
  2 s) that isn't parallel to its other tones**, so it's probably not a
  rotation tone (motor braking?). It isn't modelled.
- Spin-up times are fitted for 4 packs. The rest keep the default of 5 s.
- MicroScience's 289 Hz wobble isn't modelled.
- **Designed drives sounded artificial next to fitted ones** (13 clean sines
  against 40 measured tones). `designed_7200.json` now has a bed of 26 faint
  tones and 2 dB of flutter. Still to judge by ear.
- **Seeks:**
  - The workload's gaps are a revolution plus an exponential wait. Real ones
    come in bursts: many short gaps, then a long one.
  - Back-to-back seeks are merged by the detector, so the Ultrastar's long
    "seeks" are really runs of them.
  - Strong flutter raises the idle's 99th percentile, so the scorer finds fewer
    of the synthesis's seeks (Fireball EX).
  - There's no power-on calibration and no park on spindown yet.
  - Grains are cut at the recording's rate, so they don't yet play at other
    rates.
