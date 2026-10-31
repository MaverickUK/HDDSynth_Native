# Changelog

The HDD Synth Companion app. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org). The version is in
`src/hddsynth_companion/__init__.py`.

## 0.2.0 - 2026-10-30

### Added
- Each sample pack in the settings window has an icon for its kind: a grey
  waveform for recorded samples, an orange sine for a modelled drive
  synthesised live, with a key. The tray menu's pack items set them too,
  but macOS doesn't show them there (not yet tried on Windows).
  Needs firmware v0.24.0 (`$HDP`'s new kind field); with older firmware
  the packs have no icon.

## 0.1.0 - 2026-10-29

### Added
- Tray (menu bar) app for macOS and Windows: finds the HDD Synth on USB,
  sends the computer's disk activity (with a sensitivity setting and a live
  meter, per-disk selection, and sequential/random detection), and offers
  the pack, volume and balance. Start at login. Needs firmware v0.19.0.
- Settings window: the activity bar sits under the sensitivity slider with
  the threshold marked; a supercap bar (red/orange/green for whether a
  spindown could be played, thresholds estimated).
- About box, after HDDSynth_Win32's (logo, version, OS, the board's
  firmware, link).
- On Windows, where the USB name is often hidden, Raspberry Pi USB ports
  are checked with a status query.
