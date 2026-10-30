# Changelog

All notable changes to the HDD Synth MK5 firmware. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions
follow [Semantic Versioning](https://semver.org). Linked versions have a
release with its UF2 on the
[releases page](https://github.com/MaverickUK/HDDSynth_Native/releases).

## [0.21.0] - 2026-10-30

### Added
- **Head positions for modelled drives** (HDDSynthPro link, protocol
  version 2): `$HDE,2,<pos>,<op>,<sectors>` gives each command's target, and
  while they arrive the modelled head goes where the computer is really
  reading and writing, in place of the virtual workload. They're also
  accepted over USB, for testing.
- **`$HDM`, the modelled drive's timing,** sent back to HDDSynthPro on the
  link's other direction (J6 pin 4, unused until now) every 2 s and on each pack change, and on
  request over USB (`$HDQ,1,MODEL`). HDDSynthPro's coming `seek_timing`
  setting uses it to hold each command for as long as that drive would take.
- `src/drive_timing.*`: the seek, rotation and transfer times, host-tested
  against the drive engine, for HDDSynthPro to copy.

## [0.20.0] - 2026-10-30

### Added
- **Modelled drives:** a pack can be a physics model of a drive instead of
  recordings. Put a `drive.json` in the pack's folder and the audio core
  synthesises the drive live:
  - the spindle spins up and coasts down (hum, motor whine, bearing, air);
  - a virtual head seeks with a voice coil's or a stepper's physics, ringing
    the arm and case modes;
  - power-on unparks and calibrates, and a power cut parks the heads and
    coasts down (the spindown, from RAM).

  HDD activity drives a virtual workload (random access, or a file copy
  after 1.5 s of continuous activity, or the class the HDDSynthPro link or
  companion app sends). Four presets cover 1985 to 2005 drives, and every
  parameter can be set in drive.json (README, "Modelled drives"). The
  browser prototype used to design them, the Drive Lab, is online at
  <https://maverickuk.github.io/HDDSynth_Native/drive-lab/> (source in
  `tools/drive-lab`); the native tests check that the firmware's engine
  matches it.

### Changed
- The firmware uses about 7 KB more RAM all the time (the modelled drive's
  code runs from RAM), so the largest spindown that fits in RAM is now
  about 359 KB (11.2 s at 16 kHz). Every current pack still fits.

## [0.19.0] - 2026-10-29

### Added
- **USB companion app support:** the [HDD Synth Companion](companion/README.md)
  (a tray app for macOS and Windows, in `companion/`) sends the computer's
  own disk activity over USB, so the board needs no HDD LED wiring, and can
  choose the pack and set the volume and balance. The protocol is `$`
  sentences on the USB serial console, in the HDDSynthPro link's format
  (described in the README); the single-key console commands are
  unchanged. The app's activity adds to the activity input.

## [0.18.1] - 2026-10-28

### Fixed
- **"No SD card" (one short beep, then red flashes) at about one boot in
  five, after the PC had been off briefly.** Reported by a beta tester on
  two PCs; reinserting the card fixed it. While the PC is off the board
  runs on the supercap and writes its power-loss log to the card, and the
  SD library left that write open between sectors. When power came back
  within the supercap's hold time the board rebooted with the write still
  open; the card, still powered, kept waiting for data and ignored the next
  boot until its power was cut. Now every reboot, and every power-log
  sector, leaves the card idle.
- The card check at boot also rescues a card that a reset left
  mid-transfer, then gives the SD library's own, slower start-up a last try
  before giving up. A board halted with "no SD card" tries that too, every
  10 s, and the log records what the card answered. The SD stats count
  rescues (`rescued=`).

## [0.18.0] - 2026-10-25

### Added
- **HDDSynthPro activity link** (experimental, off unless
  `EXPERIMENTAL_HDDSYNTHPRO_SUPPORT` is on): an HDDSynthPro on header J6
  (instead of the OLED) reports the drive's activity over a one-way UART.
  Its busy flag replaces the activity input, and during sequential
  transfers an optional per-pack `sequential.wav` plays instead of
  `access.wav` (packs without one keep `access.wav`). The activity LED
  turns `SEQUENTIAL_LED_COLOUR` (orange) during them. Simulation mode mixes
  in sequential spells.

## [0.17.1] - 2026-10-25

### Fixed
- **Boot loop of beeps when the SD card wasn't ready at power-on** (seen on a
  PC: a red flash with a beep, a red flash, then a long blue beep, over and
  over, even after the PC was switched off). Two bugs together:
  - The SD card was tried once, immediately at power-on; a card that wasn't
    ready yet halted the board with "no SD card". It's now retried for up to
    3 s, and a halted board keeps looking for the card and restarts once it
    answers.
  - The buttons were only set up after the SD card mounted, so after that
    error the action button's input read as pressed, and the halted state's
    3 s long press did a factory reset and rebooted, again and again. The
    buttons are now set up first, and a factory reset from the halted state
    needs the button to have been released first.

## [0.17.0] - 2026-10-22

### Added
- **USB drive mode:** hold the action button at power-on to share the SD
  card with the computer as a drive (~450 KB/s each way), so packs and
  settings.json can be edited in place. Ejecting the drive reboots into
  normal use.

## 0.16.0 - 2026-10-22

### Added
- **Standby** (`STANDBY_MINUTES`): after that long without HDD activity the
  drive spins down and goes quiet; the next access spins it up again. A pack
  change in standby spins up. Serial `z` forces it.

## 0.15.0 - 2026-10-22

### Added
- **Per-pack `pack.json`:** `NAME` (OLED title), `AUTHOR`, `GAIN` (0.0-2.0,
  to even out pack loudness), `ACTIVITY_LED_COLOUR` and `BALANCE_DEFAULT`.

## 0.14.3 - 2026-10-22

### Fixed
- Beeps follow the volume setting (between 25% and 80%) instead of a fixed
  80%, which was jarring at low volume.

## [0.14.2] - 2026-10-18

### Fixed
- The access sound no longer seems to start from the beginning on every
  burst of regular HDD activity: after each burst it silently jumps to a
  random point in its loop.

### Added
- `scripts/flash-latest.sh` flashes a board with a release UF2.

## [0.14.1] - 2026-10-18

### Added
- Serial `o` cycles the OLED mode.

### Changed
- v0.13.0-v0.14.0 verified on hardware: OLED, low-power wait, higher sample
  rates, mixed-rate rejection, WAV metadata.

## [0.14.0] - 2026-10-17

### Added
- **Higher sample rates:** packs at 16, 22.05, 32, 44.1 or 48 kHz, one rate
  per pack (a mixed-rate pack is rejected, naming the files). The jingle may
  have its own rate.

## [0.13.2] - 2026-10-17

### Fixed
- WAVs with editor metadata before the audio are accepted: the header read
  grew from 1 KB to 4 KB.

## 0.13.1 - 2026-10-17

### Fixed
- Spinup and spindown play at the idle level (as in the CircuitPython
  firmware), so the spinup-to-idle hand-over no longer changes volume.
- Stopping a sound fades it out over 8 ms instead of cutting it (no clicks).

## 0.13.0 - 2026-10-17

### Added
- **OLED display** (SSD1306 on J6, `OLED_ENABLED`): activity graph,
  supercap graph or debug console, plus volume/balance screens for 4 s after
  a change.
- **Low-power wait** after a power-loss spindown: the audio core parks and
  the clock drops to 48 MHz, so the supercap lasts much longer.

(There is no v0.12.0: the low-power wait was developed as it and shipped in
v0.13.0.)

## [0.11.1] - 2026-10-16

### Changed
- Only USB uploads (debugging) wait 1.5 s at boot for the serial console;
  release builds boot straight away.

## [0.11.0] - 2026-10-16

### Changed
- The running board shows up over USB as "HDDSYNTH" by "HDD Synth".

## [0.10.0] - 2026-10-16

### Added
- `SAVE_VOLUME_BALANCE`: remember the volume and balance (on the SD card,
  2 s after the last change).

## [0.9.0] - 2026-10-15

### Added
- A rainbow light show on the RGB LED that pulses with the jingle.

## [0.8.0] - 2026-10-15

### Added
- `DEMO_MODE`: cycle through every pack, 60 s each, with a random LED
  colour per pack.

## [0.7.0] - 2026-10-15

### Added
- First-boot jingle (`/jingle.wav`), `ALWAYS_PLAY_JINGLE` and
  `RANDOM_SAMPLE_PACK`.

## [0.6.0] - 2026-10-15

### Added
- Beeps with RGB flashes for every operation and error, synthesised in the
  mixer.

## [0.5.0] - 2026-10-15

### Added
- Action button: short press for the next pack (no reboot, remembered),
  long press for a factory reset.

## [0.4.0] - 2026-10-15

### Added
- Rotary encoder volume and balance control.
- GitHub Actions: build, host tests and tagged releases.

## [0.3.1] - 2026-10-15

### Fixed
- No serial output after a real power loss (writing to a vanished USB host
  stalled the spindown for up to 1 s).

## [0.3.0] - 2026-10-15

### Added
- Power-loss log on the SD card (`/powerlog.bin`), printed with `l`.

## [0.2.0] - 2026-10-15

### Added
- Serial `n` to switch packs, and a test build that always streams the
  spindown.

## [0.1.0] - 2026-10-15

### Added
- The native C++ firmware: dual-core audio (SD streaming on core 0, mixing
  on core 1), spinup/idle/access/spindown, power-loss spindown from the
  supercap, watchdog, serial console, stress test.

### Fixed
- Recovery from SD card removal without blocking or rebooting.

[0.19.0]: https://github.com/MaverickUK/HDDSynth_Native/releases/tag/v0.19.0
[0.18.1]: https://github.com/MaverickUK/HDDSynth_Native/releases/tag/v0.18.1
[0.18.0]: https://github.com/MaverickUK/HDDSynth_Native/releases/tag/v0.18.0
[0.17.1]: https://github.com/MaverickUK/HDDSynth_Native/releases/tag/v0.17.1
[0.17.0]: https://github.com/MaverickUK/HDDSynth_Native/releases/tag/v0.17.0
[0.14.2]: https://github.com/MaverickUK/HDDSynth_Native/releases/tag/v0.14.2
[0.14.1]: https://github.com/MaverickUK/HDDSynth_Native/releases/tag/v0.14.1
[0.14.0]: https://github.com/MaverickUK/HDDSynth_Native/releases/tag/v0.14.0
[0.13.2]: https://github.com/MaverickUK/HDDSynth_Native/releases/tag/v0.13.2
[0.11.1]: https://github.com/MaverickUK/HDDSynth_Native/releases/tag/v0.11.1
[0.11.0]: https://github.com/MaverickUK/HDDSynth_Native/releases/tag/v0.11.0
[0.10.0]: https://github.com/MaverickUK/HDDSynth_Native/releases/tag/v0.10.0
[0.9.0]: https://github.com/MaverickUK/HDDSynth_Native/releases/tag/v0.9.0
[0.8.0]: https://github.com/MaverickUK/HDDSynth_Native/releases/tag/v0.8.0
[0.7.0]: https://github.com/MaverickUK/HDDSynth_Native/releases/tag/v0.7.0
[0.6.0]: https://github.com/MaverickUK/HDDSynth_Native/releases/tag/v0.6.0
[0.5.0]: https://github.com/MaverickUK/HDDSynth_Native/releases/tag/v0.5.0
[0.4.0]: https://github.com/MaverickUK/HDDSynth_Native/releases/tag/v0.4.0
[0.3.1]: https://github.com/MaverickUK/HDDSynth_Native/releases/tag/v0.3.1
[0.3.0]: https://github.com/MaverickUK/HDDSynth_Native/releases/tag/v0.3.0
[0.2.0]: https://github.com/MaverickUK/HDDSynth_Native/releases/tag/v0.2.0
[0.1.0]: https://github.com/MaverickUK/HDDSynth_Native/releases/tag/v0.1.0
