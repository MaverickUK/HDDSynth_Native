# Changelog

All notable changes to the HDD Synth MK5 firmware. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions
follow [Semantic Versioning](https://semver.org). Linked versions have a
release with its UF2 on the
[releases page](https://github.com/MaverickUK/HDDSynth_Native/releases).

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
