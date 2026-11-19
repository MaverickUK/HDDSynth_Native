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
  band passes through.
- **Noise:** air noise in 18 half-octave bands from 40 Hz (`db` is each
  band's RMS at full speed). Louder with speed^`speedExp`, and its bands
  shift with speed^`follow`.
- **Spindle:** `rpm`, `spinupS`, `coastS` (the same speed laws as v1),
  `jitter` (slow speed wander) and `wobble` (AM at the rotation rate).

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

## Where it stands

`score.js` against the samples:

| pack | rpm | idle level | idle bands | tones matched |
|---|---|---|---|---|
| Mark_IBM | 7267 | -1.2 dB | 1.7 dB | 100% |
| Mark_Original | 7205 | -0.3 dB | 1.4 dB | 100% |
| Mark_Toaster | 7208 | -1.1 dB | 2.0 dB | 93% |
| PB_IBM_Ultrastar | 10003 | -1.3 dB | 3.1 dB | 94% |
| PB_MicroScience612 | 3552 | -1.4 dB | 3.5 dB | 100% |
| PB_QuantumFireballEX | 5359 | -0.9 dB | 2.1 dB | 95% |
| PB_QuantumFireballTM | 4500 | -0.3 dB | 2.7 dB | 100% |
| PB_WD_Caviar2850 | 4502 | -1.4 dB | 3.1 dB | 99% |
| PB_WD_Raptor | 10004 | -0.3 dB | 1.3 dB | 99% |

"Idle bands" is the mean difference across third-octave bands.

Known gaps:
- **Fitted drives have a flat body**, so their spindowns fade evenly where
  the recordings flare as tones cross resonances. Next step: fit the body
  from the spindown, using the tracked speed.
- **Spin times are fitted only where the speed tracks cleanly** (the Raptor;
  most spindowns don't). The rest keep the defaults, to set by ear.
  `PB_IBM_Ultrastar`'s 12.5 s spin-up is doubtful.
- MicroScience's 289 Hz wobble isn't modelled.
- No actuator yet.
