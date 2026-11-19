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
- **Seeks are 10-30 ms bursts with a low "rock" (100-400 Hz)**, 10-20 a
  second and often back to back, only 1-3 dB louder than the idle overall.
  v1's were 10 dB louder, sparse and clicky.
- **The stepper drive (MicroScience) makes a tone at its step rate.**

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
  from 0.15 to 1.

Seeks, the actuator and the workloads come next.

## Tools (Node, no dependencies)

```sh
node fit.js PACK_DIR [--rpm N] [-o drive.json]   # fit a drive from a recorded pack
node render.js drive.json OUT_DIR [rate]          # render spinup/idle/spindown WAVs
node score.js SAMPLES_DIR [OUT_DIR] [--pack NAME] # fit, render and score every pack;
                                                  # OUT_DIR gets A/B WAVs to listen to
```

`dsp.js` (FFT, spectra, peaks, bands) has no Node dependencies, so the Lab
page can fit drives in the browser too.

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

## Where it stands

`score.js` against the samples:

| pack | rpm | idle level | idle bands | tones matched | spindown (flat body) | spindown (fitted) |
|---|---|---|---|---|---|---|
| Mark_IBM | 7267 | -1.2 dB | 1.7 dB | 100% | 10.0 dB | 5.9 dB |
| Mark_Original | 7205 | -0.3 dB | 1.4 dB | 100% | 12.1 dB | 7.2 dB |
| Mark_Toaster | 7208 | -1.1 dB | 2.0 dB | 93% | 5.4 dB | 5.4 dB |
| PB_IBM_Ultrastar | 10003 | -1.3 dB | 3.1 dB | 94% | 10.5 dB | 5.2 dB |
| PB_MicroScience612 | 3552 | -1.4 dB | 3.5 dB | 100% | 19.2 dB | 5.5 dB |
| PB_QuantumFireballEX | 5359 | -0.9 dB | 2.0 dB | 95% | 12.5 dB | 6.0 dB |
| PB_QuantumFireballTM | 4500 | -0.3 dB | 2.7 dB | 100% | 8.6 dB | 5.0 dB |
| PB_WD_Caviar2850 | 4502 | -1.4 dB | 3.1 dB | 99% | 10.4 dB | 6.0 dB |
| PB_WD_Raptor | 10004 | -0.3 dB | 1.3 dB | 99% | 6.9 dB | 4.8 dB |

"Idle bands" is the mean difference across third-octave bands. The spindown
columns are the same measure, frame by frame (0.25 s) over the recording,
with both sides floored at the recording's background noise. "Flat body" is
`score.js --no-spindown-fit`.

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
- **Designed drives sound artificial next to fitted ones:** 13 clean sines
  against 40 measured tones, many faint and off-integer. To do: richness
  controls (sidebands, faint tone beds, flutter).
- No actuator yet.
