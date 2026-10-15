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
  - Supercap: 1.5 F on paper. The exact fitted part, and so its ESR, is
    unconfirmed.
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

## Versioning

SemVer, in `src/version.h` (`FIRMWARE_VERSION`). Bump it in the same commit
as any firmware change (PATCH for fixes, MINOR for features, MAJOR for
incompatible SD layout or settings.json changes) and tag that commit
`vX.Y.Z`. The version is printed at boot and by the `h` serial command.

## Architecture (see README for more)

- `mixer.h/.cpp`: `Voice` and its ownership protocol
  (IDLE / PLAYING / STOP_REQ), plus `mixBlock()`. Pure C++.
- `ring_buffer.h`: lock-free SPSC ring. Free-running indices; `reset()` only
  while the voice is IDLE.
- `audio_engine.*`: core 1 loop (mix → I2S) and the core 0 control API
  (`startStream`, `startMemory`, `stop`, `setLevel`).
- `sd_streamer.*`: core 0 only. WAV parsing, ring refill, looping, retries,
  card re-init and file reopen.
- `power.*`: FLT interrupt with 3 ms confirm, supercap ADC, restore detection.
- `powerlog.*`: from power loss onwards, logs power lines to `/powerlog.bin`
  with raw sector writes so they survive the board dying; `l` prints them.
  The sector format (`powerlog_format.*`) is pure C++ and host-tested.
- `main.cpp`: state machine BOOT → SPINUP → RUN → SPINDOWN → WAIT_POWER,
  serial console, watchdog (fed only while the core 1 heartbeat advances),
  stress test.

## Status and roadmap

**Done (proof of concept, compiled and unit tested, NOT yet run on hardware):**
streaming spinup/idle/access, spindown in RAM or a primed ring, power-loss
spindown, watchdog, SD recovery, settings.json, activity input and LEDs,
serial stats and stress test.

**Next:** run the hardware test plan in the README and fix what it finds.
The first thing to confirm is that the build boots on the RP2354A.

**Still to port from the Python firmware:**
- [ ] Rotary encoder: volume / balance, with button toggling the mode
      (`rotary_enc.py`, `volume.py`, `balance.py`)
- [ ] Action button: short press = next pack, long press = factory reset
      (`action_button.py`). Pack switching should hot-swap streams, with no
      reboot or caching needed.
- [ ] Beeps and RGB flash patterns (`beep.py`, `beep_profiles.py`)
- [ ] NVM persistence: desired pack, jingle-played flag, volume/balance with
      a debounce (`nvm_wrapper.py`, `persisted_float.py`). Never write while
      audio is playing.
- [ ] Jingle on first boot; `RANDOM_SAMPLE_PACK`
- [ ] OLED (I2C on GP24/25) when the hardware is fitted
- [ ] Remove `WAIT_FOR_SERIAL_MS` for release builds
- [ ] GitHub Actions: build the UF2 and run the native tests on every push;
      attach the UF2 to tagged releases

**Open questions:**
- Exact supercap part, and spindown energy margin at high volume. Use the
  `[trace]` logs.
- Whether `PB_MicroScience612`'s 345 KB spindown fits in RAM alongside
  everything else.
