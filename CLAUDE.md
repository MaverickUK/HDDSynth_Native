# CLAUDE.md: context for working on HDDSynth Native

This repo is the native C++ firmware for the **HDD Synth MK5** board. It
replaces the CircuitPython firmware in
[MaverickUK/HDDSynth](https://github.com/MaverickUK/HDDSynth)
(`2_prototype_mk5/software/`), which remains the behavioural reference: when
porting a feature, read the matching Python module there first. Sample packs
live in [MaverickUK/HDDSynth_Samples](https://github.com/MaverickUK/HDDSynth_Samples).

The owner is an experienced software architect (C#/.NET, TypeScript, Python)
who is new to embedded C++. Explain embedded-specific decisions briefly, and
favour clear, idiomatic code over clever code.

## Why this rewrite exists

The CircuitPython firmware froze, or skipped the spindown, when streaming
samples from SD. The root cause is a race inside CircuitPython 10.x:

1. `MixerVoice.play()` marks the voice active, then reads the first buffer
   from SD in the Python thread.
2. The SPI driver runs background tasks while it waits, so the audio refill
   runs nested mid-read. FatFs is non-reentrant (`FF_FS_TINY=1`, one shared
   sector buffer) and the SPI bus is already locked, so the nested read fails
   with EAGAIN. The file is then stuck in an error state.
3. The mixer ignores `GET_BUFFER_ERROR`. A looping voice then spins forever
   in the background task: a hard freeze. A non-looping voice (spindown) just
   ends silently.

This only happens at transitions (spinup → idle/access, power loss →
spindown), never in steady looping. Caching to internal flash avoided it.

**Design rule that prevents it here:** core 0 is the *only* code that touches
the SD card. Core 1 only reads RAM. They communicate solely through SPSC ring
buffers and per-voice atomic state. Never let core 1 (or an interrupt) do SD
or file I/O, and never block core 1.

## Hardware facts (MK5)

- **MCU:** RP2354A, an RP2350A with **2 MB in-package flash** on QSPI CS0.
  Built as `rpipico2` with `board_upload.maximum_size = 2097152`.
  - There's an unpopulated SOIC-8 footprint (U3). Its CS goes to QSPI_SS via
    R7 (0 Ω, fitted) or to GPIO0 via R6 (DNF), with R9 (DNF) as a pull-up.
  - **Do not fit a flash chip on CS0** alongside the internal flash.
  - Fitting APS6404L PSRAM on CS1/GPIO0 is a possible upgrade.
- **Pins:** see `src/config.h` (they match the Python `settings.py`).
  - SD on SPI1: SCK 10, MOSI 11, MISO 8, CS 9.
  - I2S: BCLK 13, LRCLK 14, DATA 12.
  - Amp SD_MODE: GP15.
- **Amp:** MAX98357A. GAIN_SLOT is tied to VSYS (6 dB); SD_MODE high selects
  the left channel.
- **Power path:**
  - 5 V → TPS25921A eFuse → VIN_PROT → TPS61094 (supercap backup) → VSYS
    (amp) → TPS63020 buck-boost → +3V3 (MCU and SD).
  - eFuse current limit ≈ 1.16 A (R36 110 k); UV trip ≈ 4.1 V rising /
    4.0 V falling.
  - TPS61094 boost holds VSYS at 4.2 V (OSEL 75 k); supercap charge
    termination 4.15 V (VCHG 75 k); charge current ≈ 250 mA.
  - Supercap: the schematic says a 1 F 5.5 V radial part (Kitronik). The
    exact fitted part, and so its ESR, is unconfirmed.
  - Power-loss signal: eFuse /FLT on GP28, open drain with 10 k pull-up, LOW
    on fault. Once the input has fully decayed, FLT floats back high, so
    "power restored" also requires the supercap to be rising (see `power.cpp`).
  - Supercap sense: GP27 via a 22 k/22 k divider.
- **Buttons/debug:** S1 = RUN (reset), S2 = USB_BOOT. J2 = SWD (SWCLK, GND,
  SWDIO).
- **Known board issues (for the next revision):**
  - SD DAT1/DAT2 (pins 8 and 1) are floating and should have pull-ups.
  - No series termination on the SD SPI lines.
  - TPS63020 PG and the SD card-detect pin aren't connected.
  - VSYS bulk capacitance is thin (about 74 µF nominal MLCC).
  - **Supercap stopped holding charge after one deep discharge** (seen
    2026-10-15, hardware test 6, powered from a Mac's USB-C port). A real
    power cut ran it down to about 1.1 V before the board died. Afterwards
    it stayed at 0 V on USB for 6+ minutes; it should charge at about
    0.17 V/s. Before that cut it held 4.1 V.
    - Measured with a multimeter while running: VIN_PROT = 5.12 V and the
      supercap = 0 V.
    - So the input has headroom: the TPS61094 in auto mode needs VIN above
      about 4.3 V (boost target 4.2 V, OSEL 75 k, plus 0.1 V).
    - Leading hypothesis: the supercap failed short. The schematic part is
      a "1F 5.5V" radial supercap (Kitronik), typically two unbalanced
      cells in series. Draining it to near 0 V through the boost converter
      can reverse-charge the weaker cell.
    - Not yet confirmed. The test: remove the cap and check whether its
      pads charge to ~4.15 V, and whether the cap holds charge off-board.
    - For the next revision: decide whether a series supercap should ever
      be fully drained (cell balancing, or a cut-off before it does). In
      firmware, consider shutting down once the spindown is done and the
      supercap falls below about 2 V, rather than letting the boost drain
      it. After the first cut the board ran 53 s past the spindown.

## Toolchain notes and gotchas

- **Toolchain:** PlatformIO + Arduino-Pico (earlephilhower core) via the
  maxgerhardt platform. I2S and SdFat are bundled with the core; ArduinoJson
  comes from GitHub via `lib_deps`.
- **No float `printf`:** Arduino-Pico links newlib-nano without float
  formatting, so `%f` prints nothing. Log integers (mV, %, µs) instead.
- **SD CRC:** enabled with `-DUSE_SD_CRC=1`.
- **Flash writes stall core 1:** Arduino-Pico pauses the other core during
  flash writes (EEPROM/LittleFS). Never write flash while audio is playing;
  only during boot, or with audio deliberately stopped.
- **I2S IRQ placement:** `i2s.begin()` is called in `setup1()` so its DMA
  IRQ is serviced on core 1.
- **SD card across resets:** the card stays powered through any reset of
  the RP2350, and with `DEDICATED_SPI` SdFat leaves multi-block reads and
  writes open between calls. A card reset mid-write ignores CMD0 until its
  power is cut: "no SD card" until it's reinserted (a beta tester hit this
  at ~1 boot in 5 after short PC power-offs, v0.18.0; reproduced 7 of 8
  simulated cuts, where our card just needed a second try). So every planned
  reboot goes through `reboot()` in main.cpp (and usb_drive.cpp calls
  `sd::finishTransfers()`), `sd::writeSector()` closes each write, and
  `cardResponds()` sends the stop token and CMD12 before retrying CMD0 (for
  unplanned resets; not yet exercised on hardware, as our card never needed it).
- **Fast code on core 1:** code in flash runs through the 16 KB XIP cache
  that both cores share, and a big per-sample loop there stalls on flash
  reads (the drive engine took 4.7 ms per 4 ms block). Put the hot path in
  RAM with `__attribute__((section(".time_critical.<name>")))`, and force
  its small helpers inline: left out of line, the compiler puts them in
  flash and calls them through veneers (check the disassembly for `bl`
  to `..._veneer`). Avoid `sqrtf()` (it keeps errno, so it's a software
  routine that also links ~5 KB of float helpers into RAM; use `vsqrt.f32`,
  see `sqrtFast()`), `tanf()`/`powf()` per sample, doubles (the FPU is
  single precision), and float-to-64-bit conversions (`__aeabi_f2ulz`).
  `mix_max` in `[stats]` is the worst block's mix time.
- **Unit tests:** host-side tests run with plain `c++` (not `pio test`):
  `test/run_native_tests.sh [path/to/HDDSynth_Samples]`. They use
  ASan/UBSan; also run them under TSan when touching the ring buffer or the
  voice handshake. Keep `wav`, `mixer` and `ring_buffer` free of Arduino
  dependencies so they stay testable on the host.

## USB identity

The running firmware reports product "HDDSYNTH", manufacturer "HDD Synth"
(`board_build.arduino.earlephilhower.usb_*` in platformio.ini). The VID/PID
stay Raspberry Pi's 0x2E8A/0x000F so picotool and uploads work. The BOOTSEL
drive name ("RP2350") and its USB strings come from the boot ROM; RP2350
can white-label them via OTP (`USB_WHITE_LABEL`), but OTP is permanent, so
only do it deliberately, per board, with the owner's say-so.

## Versioning

SemVer, in `src/version.h` (`FIRMWARE_VERSION`). Bump it in the same commit
as any firmware change (PATCH for fixes, MINOR for features, MAJOR for
incompatible SD layout or settings.json changes) and tag that commit
`vX.Y.Z`. The version is printed at boot and by the `h` serial command.
Add the version to `CHANGELOG.md` (Keep a Changelog style) in the same
commit. Push the tag on its own after the branch (`git push origin main`, then
`git push origin vX.Y.Z`): a tag pushed together with its branch didn't
trigger the release workflow.

## Architecture (see README for more)

- `mixer.h/.cpp`: `Voice` and its ownership protocol
  (IDLE / PLAYING / STOP_REQ), plus `mixBlock()`. Pure C++.
- `ring_buffer.h`: lock-free SPSC ring. Free-running indices; `reset()` only
  while the voice is IDLE.
- `audio_engine.*`: core 1 loop (mix → I2S) and the core 0 control API
  (`startStream`, `startMemory`, `stop`, `setLevel`).
- `sd_streamer.*`: core 0 only. WAV parsing, ring refill, looping, retries,
  card re-init and file reopen.
- `controls.*`: rotary encoder (pin interrupts on core 0), its button and the
  action button (`button.h`: debounce and short/long press, host-tested).
- `persist.*`: saved pack and jingle flag in the EEPROM-emulation flash
  sector; `persist_format.*` is the CRC-checked record (host-tested).
- `drive_engine.*`, `drive_params.*`, `drive_workload.h`: the modelled drive
  (pure C++, host-tested), a port of `tools/drive-lab/engine.js` (change
  both together; `compare.js` checks them). It renders as `VOICE_DRIVE`'s
  source in `mixBlock()`; core 0 sends commands through its SPSC ring and
  `updateParams()`. `drive_pack.*` parses `drive.json`. main.cpp's
  `modelled` flag switches spin-up, run, spindown, standby and demo over to
  it (`startDrive()`, `spindownPlaying()`).
- `console.*`: **print through `Console`, not `Serial`.** It writes to USB
  serial and keeps the last lines for the OLED debug screen. (Reading serial
  commands still uses `Serial`.)
- `power.*`: FLT interrupt with 3 ms confirm, supercap ADC, restore detection.
- `powerlog.*`: from power loss onwards, logs power lines to `/powerlog.bin`
  with raw sector writes so they survive the board dying; `l` prints them.
  The sector format (`powerlog_format.*`) is pure C++ and host-tested.
- `main.cpp`: state machine BOOT → SPINUP → RUN → SPINDOWN → WAIT_POWER,
  serial console, watchdog (fed only while the core 1 heartbeat advances),
  stress test.

## Status and roadmap

**Done and run on hardware (2026-10-15):** the README's hardware test plan
(steady state, stress, SD removal, simulated power loss, every pack on the
test card: 8 packs; not yet `Mark_Original` or `PB_IBM_Ultrastar`) plus every feature
below. Still open from it: real power cuts (one worked; then the supercap
stopped charging, see known board issues). The test plan has since been removed from
the README; its findings are in the git history and the known board issues
above.

**Settings:** every settings.json key is documented in the README table.
Keep it in sync when adding or changing a key (`settings.cpp`).

**New features (not in the Python firmware):**
- `DEMO_MODE` (settings.json): cycle through every pack, 60 s each
  (`cfg::DEMO_PACK_MS`), with the real spindown between packs
  (`DEMO_SPINDOWN` state) and a random hue for the activity LED. Forces
  simulated activity. Never writes flash (a write per minute would wear the
  sector out). Serial `m` toggles it for testing.
- Low-power wait after a power-loss spindown (`enterLowPower()` in main.cpp):
  core 1 parks (I2S stopped, WFE), then clk_sys drops to 48 MHz via
  `set_sys_clock_48mhz()`, LEDs and OLED off. One-way: the exit is a reboot.
  Cut 1 drew ~130 mW in this wait; the aim is a fraction of that. Verified
  2026-10-18 with simulated losses (parks, 48 MHz, power log keeps writing,
  clean restart); the actual current saving isn't measured yet.
- Sample rates: packs may be 16000, 22050, 32000, 44100 or 48000 Hz
  (`WAV_SUPPORTED_RATES` in wav.h), one rate per pack (`checkPackRate()` in
  main.cpp). The I2S output switches per pack (`audio::setSampleRate()`,
  applied by core 1 between blocks); the jingle may have its own rate. Beeps
  are made at the current rate. `cfg::DEFAULT_SAMPLE_RATE` (16 kHz) is only
  the boot rate. Higher rates shrink the spindown-in-RAM limit and the ring
  cover (rings are 16384 samples: ~0.34 s at 48 kHz). Verified 2026-10-18
  with resampled packs at 22.05/32/48 kHz (streamed spindown at 48 kHz kept
  `under=0`, min ring 320 ms of 340), a mixed-rate pack rejected by name, and
  a WAV with 3 KB of metadata loading.
- Access loop randomised: it plays continuously at level 0 between bursts
  (as in Python), and 50 ms after each burst ends (`cfg::ACCESS_JUMP_QUIET_MS`)
  `jumpAccess()` re-primes it from a random point (~20 ms of SD reads).
  Prompted by a PC with a burst every ~10 s against ~10 s loops: every
  burst sounded like the loop's start. Verified 2026-10-18 (demo mode).
- `pack.json` per pack (`pack_info.*`, loaded by `loadPackInfo()` in main.cpp):
  NAME (OLED title), AUTHOR, GAIN 0.0-2.0 (scales spinup/idle/access/
  spindown; `levelFromFloat()` now allows up to ~2.0, host-tested),
  ACTIVITY_LED_COLOUR, BALANCE_DEFAULT (precedence: settings.json > saved at
  boot > pack.json > 0.5; on a pack change the pack's applies unless
  settings.json pins it). `json_colour.h` shares the colour parser.
- Standby (`STANDBY_MINUTES`, 0 = off, ignored in demo mode): no activity for
  that long → `enterStandby()` plays the spindown (STANDBY_SPINDOWN), amp off,
  STANDBY. Activity → `wakeFromStandby()` → `startSpinup()`. A pack change
  in standby spins up (`changePack(true)`). Power loss in STANDBY skips the
  spindown. Serial `z` forces it. Verified 2026-10-22 on hardware.
- USB drive mode (`usb_drive.*`): action button held at power-on (checked in
  `boot()` right after settings.json is read, before anything writes to the
  card) → TinyUSB mass-storage interface registered with Arduino-Pico's
  `USB` (the `FatFSUSB` pattern, but raw SD sectors via `sd::readSectors()`
  / `writeSectors()`). 64-byte USB slices: 4 KB read-ahead, writes gathered
  per sector. The loop runs `tud_task()` itself under `USB.mutex` (the core's
  1 ms timer alone gave ~64 KB/s; now ~450 KB/s). macOS signals eject with
  START STOP UNIT → reboot. Verified 2026-10-22 (mount, read, write, eject).
- Jingle light show: while the jingle plays, the RGB LED cycles the rainbow
  (`cfg::JINGLE_RAINBOW_MS`) and its brightness follows the loudness. Core 1
  publishes each mixed block's peak (`audio::outputPeak()`); core 0 smooths
  it with `LevelFollower` (fast attack, slow release, auto-gain; host-tested).
  Serial `j` replays the jingle (clears its flag and reboots).

- HDDSynthPro activity link (`EXPERIMENTAL_HDDSYNTHPRO_SUPPORT`, v0.18.0),
  from HDDSynthPro's `docs/hddsynth-activity-link-brief.md`: UART1 RX on
  J6 pin 3 (GP25; the OLED is forced off), `$HDA` sentences parsed by
  `link_parser.*` (pure, host-tested), received by `hdd_link.*` on core 0.
  While present, `io::activity()` uses its busy flag and
  `io::activityClass()` its class; `AccessChooser` picks `sequential.wav`
  (`VOICE_SEQUENTIAL`, ring heap-allocated only with the flag, 32 KB) or
  `access.wav`; `SEQUENTIAL_LED_COLOUR` (orange: full yellow looked green on the LED). Simulation mixes in
  sequential spells. Verified 2026-10-25 in simulation (sequential.wav
  streams with no underruns, the LED and sample switch); not yet with real link
  lines or HDDSynthPro.

- USB companion app (`companion/`, Python + PySide6, its own version; v0.19.0
  firmware): `$` lines on the USB console go to `usb_host.*` (assembled by
  `LineAssembler` in `handleSerial()`, so `-` inside a sentence isn't
  "volume down"). `$HDA` is activity (ORed with the local input in
  `io::activity()`), `$HDQ`/`$HDC` are parsed by `host_protocol.*` (pure,
  host-tested) and run by `runHostCommand()` in main.cpp; replies via
  `usbhost::send()` straight to Serial (not Console), waiting up to 50 ms
  for buffer room. `sentence.*` holds the shared NMEA helpers (also used by
  `link_parser`). Pack by index: `changePack(spinUp, index)` →
  `loadPackAt()`. `$HDS` is pushed on change while the app is active.
  The app's tests (`companion/tests`, pytest) share known answers with the
  firmware's; CI: `.github/workflows/companion.yml`. Verified 2026-10-29 on
  macOS against the board (protocol script: status, pack list, VOL/BAL
  saved, PACK switch and range error, bad checksum ignored, `-` in a
  sentence harmless; the app connects, switches packs, follows the encoder).
  Not yet run on Windows. The app's supercap thresholds
  (`companion/.../supercap.py`) are estimates: measure a real cut.
  macOS gotcha: QFormLayout keeps fields at their size hint, so custom
  widgets need a sizeHint() (the supercap bar was 0 px wide).

- Modelled drives (`drive.json`, v0.20.0): see the README. Designed in the
  browser prototype (`tools/drive-lab`), published to GitHub Pages by
  `.github/workflows/pages.yml` (https://maverickuk.github.io/HDDSynth_Native/drive-lab/).
  Verified 2026-10-30 on hardware with the four presets (packs
  `MODEL_*` on the test card): ~0.9-1.05 ms per 4 ms block at 32 kHz with
  simulated activity, no underflows, the modelled spindown on a simulated
  power loss (8.3 s for `voice97`). Next (the plan's phase 3): `$HDE` head
  positions from HDDSynthPro, live tuning from the companion app
  (`$HDC,1,SET`), and optional recorded grains (seek, park) per pack.

**Still to port from the Python firmware:**
- [x] Rotary encoder: volume / balance, with button toggling the mode
      (`controls.*`, decoding in `quadrature.h`).
- [x] Action button: short press = next pack (hot swap, no reboot, saved),
      long press = factory reset (clears saved state, reboots).
- [x] Beeps and RGB flash patterns: `beep.*` synthesises them in the mixer
      (no buffer), `beeps.*` holds the profiles and drives the LED. Serial
      `k` auditions them. (Python's yellow "busy" flashing at boot and the
      RGB startup test are not ported.)
- [x] NVM persistence: desired pack and jingle flag (`persist.*`). Flash
      writes pause core 1, so only write with the audio stopped and the amp
      muted (e.g. on a pack change).
- [x] Saving volume/balance (`SAVE_VOLUME_BALANCE`, opt-in; `saved_levels.*`,
      `levels_format.*`). Stored on the **SD card** (`/state.bin`, two
      CRC'd slots), not flash: a flash write pauses core 1 for ~40 ms while
      idle/access play. 2 s debounce. `VOLUME_DEFAULT`/`BALANCE_DEFAULT` in
      settings.json override saved values. Factory reset clears them, even
      with saving off. (Originally declined over the flash glitch; added
      2026-10-16 once SD storage avoided it.)
- [x] Jingle on first boot (`/jingle.wav`, full volume, `JINGLE` state before
      spinup; `ALWAYS_PLAY_JINGLE` is a settings.json key here);
      `RANDOM_SAMPLE_PACK` (avoids the previous pack, saves its choice).
      RANDOM_SAMPLE_PACK is not yet tried on hardware.
- [x] OLED (SSD1306 on J6, I2C0 GP24/25), opt-in with `OLED_ENABLED`; modes
      `activity`/`supercap`/`debug`, plus volume/balance screens for 4 s
      after a change. Verified on hardware 2026-10-18 (all modes, the
      overlays, orientation); serial `o` cycles the mode. Drawing is pure C++
      (`canvas.*`, `oled_screens.*`, `oled_model.h`, host-tested, previews in
      `docs/oled/`); `oled.*` is the I2C driver and screen logic. A frame
      blocks core 0 ~25 ms at 400 kHz, so frames are rate-limited. The panel
      switches off at power loss.
- [x] `WAIT_FOR_SERIAL_MS` (1.5 s boot wait for the console) only in USB
      uploads: `scripts/serial_wait.py` adds it to project sources when the
      `upload` target is used. CI and release UF2s don't wait.
- [x] GitHub Actions (`.github/workflows/build.yml`): builds both envs and
      runs the native tests against the sample packs on every push/PR; a
      `vX.Y.Z` tag must match `FIRMWARE_VERSION` and publishes a release
      with the UF2

**Open questions:**
- Exact supercap part, and spindown energy margin at high volume. Use the
  `[trace]` logs.
- ~~Whether `PB_MicroScience612`'s spindown fits in RAM~~: yes, tested
  2026-10-16 (v0.9.0). Its 11 s spindown loads into RAM leaving 45 KB free,
  13 KB above `HEAP_RESERVE_BYTES`; it passed a 60 s stress test. **RAM
  budget:** v0.20.0's modelled drive took ~7 KB of that margin (its code in
  RAM), leaving ~6 KB: a new feature that uses more than that pushes this
  pack's spindown onto the streamed fallback (works, but check it).
  `EXPERIMENTAL_HDDSYNTHPRO_SUPPORT` (32 KB ring) already does.
