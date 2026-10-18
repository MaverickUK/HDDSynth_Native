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
Push the tag on its own after the branch (`git push origin main`, then
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
- Jingle light show: while the jingle plays, the RGB LED cycles the rainbow
  (`cfg::JINGLE_RAINBOW_MS`) and its brightness follows the loudness. Core 1
  publishes each mixed block's peak (`audio::outputPeak()`); core 0 smooths
  it with `LevelFollower` (fast attack, slow release, auto-gain; host-tested).
  Serial `j` replays the jingle (clears its flag and reboots).

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
  budget:** a new feature that uses more than ~13 KB of RAM pushes this
  pack's spindown onto the streamed fallback (works, but check it).
