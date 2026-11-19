# Drive Lab v2 (work in progress)

A rebuild of the modelled drive, aimed at sounding like the real recordings in
[HDDSynth_Samples](https://github.com/MaverickUK/HDDSynth_Samples). It will
replace `tools/drive-lab` (and the firmware's `src/drive_engine.*` port) once
it's ready. Until then v1 stays as it is.

**A drive is always pure synthesis at heart.** It's a drive.json of numbers
(about 3-5 KB), and every sample is made live from it, so it can answer the
PC's disk activity as the IDE Synth Bay reports it. There are two ways to make
one, both on the Lab page:
- **Design a drive** from scratch: choose its hardware (size, speed, platters,
  motor, bearing, arm, parking, mounting), and `design.js` builds the model.
- **Import a sample pack**: the page fits a model to the pack's recordings
  (`fit.js`, run in the browser), to hear against them, edit and save.

Recorded seek sounds (grains) are an optional extra for an imported drive,
never required.

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

### Head events

What the heads do as the power changes, as two scripts played by the
actuator: `heads.spinup` (from power on) and `heads.spindown` (from power off).
Each event is `{ t, ms, db, x? }`:
- `t` is when, in seconds after the power changed.
- `ms` is how long the move takes.
- `db` is its level against an ordinary seek of that length.
- `x`, if given, is where it goes (0 is the outer edge, where a voice coil
  parks). Without it, the head moves as far as a seek of `ms` would.

While a script plays, the workload waits for it. There can be up to 128 events
per script.

The recordings have the same kinds of event, at times that vary from pack to pack:
- **The motor starting (0-1.4 s):** one or two kicks of 30-180 ms. On the
  Caviar, also a run of ticks that speed up with the motor.
- **Loading the heads and calibrating, near full speed:** a run of seeks.
  - The Caviar has one every 60 ms for 3 s.
  - The Ultrastar has a sweep of short steps every 10 ms.
  - The Fireball EX has a few long seeks, then a rhythm of 130 ms.
- **The park at power off (0-0.1 s):** a strong move, sometimes followed by a
  latch click.
- **Sometimes a knock later in the spindown:** Mark_Original has a loud 90 ms
  one at 1.9 s.

`head_fit.js` finds the events and fits them:
- **Finding them:** the background is each third-octave band's running median
  over ±200 ms, since the speed changes all the way through. An event is a
  burst 10 dB over that background. It must light at least four bands, or
  three if it's short: tones sweeping through a band light only one or two.
- **Fitting them:** each event becomes a move at its own time. Its time and
  level are corrected over five rounds: render the events alone, add them to
  the recording's background, and measure that the same way.
- Designed drives get theirs from `design.js` (see Designing a drive).
  `drives/designed_7200.json` is an earlier, hand-written example.

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

## Designing a drive (`design.js`)

`designDrive(choices)` turns plain choices about the hardware into the model.
The choices are listed in `DESIGN_DEFAULT`, and there are six starting points
in `PRESETS`, from a 1986 stepper to a 2008 laptop drive. The mapping follows
the physics where there is some:
- **Motor:** tones at the commutation rate (6 per pole pair per turn) and its
  harmonics, the slot rate, and the cogging (the lcm of slots and poles).
- **Ball bearings:** the defect tones from the bearing's geometry (cage, ball
  spin, outer and inner race), louder with wear, plus a rumble. A fluid
  bearing has only a faint whirl at just under half the rotation rate.
- **Body:** three resonances, lower for bigger drives and more platters, then
  shaped by the mounting. Bare on a desk loses the low one; a PC case adds a
  boom at 140 Hz; a rubber sled damps them all.
- **Air:** noise peaking higher and louder with speed, louder with more
  platters.
- **Arm:** a rock at 210/330/560 Hz (5.25/3.5/2.5 inch), lower with more
  platters, and its overtones.
- **Head events:**
  - a start-up kick (harder for heads stuck to a landing zone)
  - ticks for old motors
  - the heads loading off the ramp or the landing zone
  - calibration strokes, a sweep and a few seeks
  - at power off: the park and latch, a skid for landing-zone drives, and
    a knock at the stop
- **Variety:** a seed varies every level by up to ±2 dB and places the tone
  bed, so "Another like it" gives the same hardware with different small
  details.

`heads.rest` is where the head sits with the power off: -0.06 on a ramp just
off the platter, or 1 on the landing zone.

Last, it renders a little and sets the levels:
- the idle to -20 dBFS RMS, as recorded packs are
- a full-stroke seek to -4 dBFS (plus the design's seek loudness)
- no head event above -3 dBFS

The choices are kept in `drive.design`, so a saved design reopens with its
controls. It takes 100-250 ms, so the page rebuilds the model live as a
control moves.

## Tools (Node, no dependencies)

```sh
node fit.js PACK_DIR [--rpm N] [-o drive.json]   # fit a drive from a recorded pack
node render.js drive.json OUT_DIR [rate]          # render spinup/idle/access/spindown WAVs
node score.js SAMPLES_DIR [OUT_DIR] [--pack NAME] # fit, render and score every pack;
                                                  # OUT_DIR gets A/B WAVs to listen to
node grains.js PACK_DIR drive.json [--max N]      # optional: add recorded seeks to a drive
```

The modules have no Node dependencies, apart from reading files (`fitPack()`,
`readWav()`). So the Lab page runs the same code: `fitAudio()` fits from
samples already in memory, and `parseWav()` and `encodeWav()` work on bytes.

### The Lab page

```sh
python3 build.py              # -> drive-lab.html (the page body, as v1's)
python3 build.py --site DIR   # -> DIR/index.html, a full page to open in a browser
```

`build.py` wraps every module in one `__bundle()`: a small CommonJS with
`require` and `module.exports`, with `fs` and `path` stubbed. The page calls
it. The fitting worker gets its source as text, as the AudioWorklet gets
`makeEngine`.

**Left: the drive itself.**
- **The top view:** the platters spin at the drive's real speed, and the arm
  swings to the head's place. It shows the ramp or the landing zone, a
  stepper or a voice coil, and the activity LED. Click the platter to seek
  there, as the Bay's head positions do.
- **"What the PC is doing":** nothing, random reads, copying a file, or
  bursts. These are the kinds of activity the Bay reports.
- **What you're hearing:** a live spectrum, with ticks marking where the
  drive's tones are now, and a spectrogram.

**Right: two tabs.**
- **Design a drive:**
  - pick a starting point, then six illustrated sections (platters and speed,
    the spindle motor, the bearing, the heads and arm, power on and off, the
    case and mounting)
  - each section says what it models and what you'll hear, with two to four
    controls
  - "Another like it" changes the seed
- **Import a sample pack:**
  1. Choose a folder, or files, or drop them on the page. It shows which WAVs
     it found and what each one gives the model.
  2. **Build the model:** a worker runs `fitAudio()` and shows each step. It
     then cuts grains from access.wav and renders the model at the pack's rate.
     It takes about 15 s for a 16 kHz pack.
  3. **Listen:** play the recording or the model, for each of the spin-up,
     spinning, seeking and spindown. Choose the seek sounds (synthesised,
     half and half, or recorded), then download drive.json and, if you chose
     recorded seeks, the grains WAV.

  The nine drives fitted from HDDSynth_Samples are there as examples.
- **Advanced** (folded away): every number in the model, as tables and fields.
  The design rebuilds these whenever a design choice changes.

**Saving.** Save writes drive.json. A drive whose seeks are all synthesised is
saved without its `grains`, so it needs nothing else. Open takes a version 3
drive.json: a designed one opens in the Design tab with its controls, and any
other in the Import tab.

It isn't on GitHub Pages yet; v1 stays there until v2 replaces it.

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
6. **Head events** (`head_fit.js`): from spinup.wav and spindown.wav, played by
   the fitted actuator (see Head events above).

## Where it stands

`score.js` against the samples:

| pack | rpm | idle level | idle bands | tones matched | spindown (flat body) | spindown (fitted) | seeks/s real / synth | busy real / synth | seek level | seek spectrum | head events spin-up | head events spindown | head event level |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Mark_IBM | 7267 | -1.2 dB | 1.9 dB | 100% | 10.0 dB | 5.8 dB | 64.5 / 55.6 | 33% / 34% | +0.7 dB | 7.4 dB | 5/5 | 5/5 | 6.4 dB |
| Mark_Original | 7205 | -0.4 dB | 1.3 dB | 100% | 12.1 dB | 6.4 dB | 31.5 / 28.1 | 28% / 30% | 0.0 dB | 5.7 dB | 11/12 | 3/4 | 7.6 dB |
| Mark_Toaster | 7208 | -1.2 dB | 2.0 dB | 93% | 5.4 dB | 5.6 dB | 12.7 / 11.2 | 46% / 41% | -0.7 dB | 3.5 dB | 4/7 | 9/14 | 7.7 dB |
| PB_IBM_Ultrastar | 10003 | -1.8 dB | 3.4 dB | 76% | 10.5 dB | 5.4 dB | 8.6 / 5.6 | 70% / 76% | +0.6 dB | 4.7 dB | 30/38 | 4/4 | 5.8 dB |
| PB_MicroScience612 | 3552 | -1.8 dB | 3.7 dB | 90% | 19.2 dB | 5.8 dB | 12.1 / 15.5 | 28% / 30% | +0.6 dB | 4.7 dB | 35/37 | 2/2 | 3.3 dB |
| PB_QuantumFireballEX | 5359 | -1.0 dB | 2.3 dB | 82% | 12.5 dB | 5.9 dB | 38.6 / 38.0 | 23% / 29% | +1.1 dB | 3.9 dB | 17/20 | 5/7 | 4.9 dB |
| PB_QuantumFireballTM | 4500 | -0.3 dB | 2.7 dB | 100% | 8.6 dB | 4.9 dB | 9.1 / 9.7 | 6% / 6% | +0.7 dB | 6.0 dB | 29/33 | 11/12 | 6.6 dB |
| PB_WD_Caviar2850 | 4502 | -1.3 dB | 3.1 dB | 98% | 10.4 dB | 5.9 dB | 20.1 / 29.0 | 21% / 29% | -0.2 dB | 2.8 dB | 68/74 | 3/5 | 3.6 dB |
| PB_WD_Raptor | 10004 | -0.4 dB | 1.2 dB | 97% | 6.9 dB | 5.0 dB | 13.5 / 15.2 | 14% / 17% | +0.1 dB | 4.5 dB | 17/18 | 5/8 | 6.7 dB |

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
- **Head events:** the recording's events matched by the render's within
  20 ms, out of the recording's. Both are found with `head_fit.js`, each
  against its own background. The level column is the mean difference of the
  matched events. It's larger than inside the fit, which measures against the
  recording's own background: a synthesised spindown's tones sweep through
  different bands, which hides some events and adds others.

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
  - Stepper seeks ignored their level (`levelExp` and, for head events, `db`)
    until the head events found it. Refitted, the Caviar's seeks are now
    detected too often (29/s against 20/s).
  - The workload's gaps are a revolution plus an exponential wait. Real ones
    come in bursts: many short gaps, then a long one.
  - Back-to-back seeks are merged by the detector, so the Ultrastar's long
    "seeks" are really runs of them.
  - Strong flutter raises the idle's 99th percentile, so the scorer finds fewer
    of the synthesis's seeks (Fireball EX).
  - Grains are cut at the recording's rate, so they don't yet play at other
    rates.
