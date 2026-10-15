# HDDSynth Native

Native C++ firmware for the [HDD Synth](https://github.com/MaverickUK/HDDSynth) MK5 board (RP2354A), replacing the CircuitPython firmware. Licensed under GPLv3, like the HDD Synth software.

## Status: proof of concept

A dual-core C++ firmware for the MK5 board (RP2354A), replacing the
CircuitPython firmware. This proof of concept covers the risky core:
SD streaming, mixing, spinup/spindown and power loss.

## Architecture

| Core | Owns | Never does |
|---|---|---|
| **Core 1** | Mixer and I2S output. Reads RAM only. | Touch the SD card, block, allocate |
| **Core 0** | SD card (sole owner), power monitoring, LEDs, activity, serial console | Generate audio |

The two cores share only lock-free ring buffers (about 1 s of audio per
streamed voice) and a small per-voice state handshake (see `src/mixer.h`).
Because only core 0 ever touches the SD card, the CircuitPython race cannot
occur. An SD stall or read error just drains the ring while core 0 retries
or re-initialises the card.

- **Spinup, idle, access** stream from SD. The idle/access rings are filled
  while spinup plays, so the handover is seamless.
- **Spindown** is loaded into RAM at boot if it fits (leaving 32 KB spare).
  Otherwise it streams from a ring that is kept pre-filled, so it starts
  instantly either way.
- **Power loss** comes from the eFuse FLT pin: an interrupt plus a 3 ms
  confirmation, instead of 50 ms+ polling.
- **Safety:**
  - Hardware watchdog (2 s), fed only while the audio core's heartbeat is
    advancing.
  - SD transfers are CRC-checked (`USE_SD_CRC=1`).
  - Level changes ramp over one 8 ms block, so there are no clicks.

SD card layout and `settings.json` keys are the same as the CircuitPython
version (`/samples/<pack>/{spinup,idle,access,spindown}.wav`). Add
`"POWER_DETECTION": false` to run from USB only, with no power-loss detection.

Not in this proof of concept yet: rotary encoder, action button / pack
switching, beeps, NVM persistence, jingle, OLED.

## Build and flash

One-time setup: VS Code + the PlatformIO extension (or `pip install platformio`).

```sh
pio run                  # build -> .pio/build/mk5/firmware.uf2
pio run -t upload        # build and flash
pio device monitor       # serial console, 115200
```

**First flash** (the board is running CircuitPython):
1. Hold **S2 (BOOT)**, tap **S1 (RESET)**, release S2.
2. A drive called `RP2350` appears.
3. Run `pio run -t upload`, or drag `firmware.uf2` onto the drive.

After that, uploads reset the board into the bootloader automatically. The
serial command `b` also does it.

**Back to CircuitPython:** same BOOT/RESET dance, then drag the CircuitPython
UF2 back on. Your CIRCUITPY files are untouched by this firmware.

### Unit tests (run on your Mac, no hardware)

```sh
test/run_native_tests.sh ../HDDSynth_Samples   # optional: path to sample packs
```

Covers:
- WAV parsing, including every file in the sample pack repo.
- The ring buffer and the mixer, under AddressSanitizer/UBSan.
- Two-thread producer/consumer tests that mirror core 0 and core 1.

## Serial console

```
h  help
s  stats now (also printed every 5 s in RUN)
t  stress test on/off: restarts streamed voices every 30-250 ms
p  simulate power loss (runs the spindown path)
a  toggle simulated HDD activity
+  / -  volume
r  reboot
b  reboot into the USB bootloader
```

Stats lines look like:

```
[stats] t=65s state=RUN heap=301234 mix_max=95us i2s_underflow=0 | idle: on under=0 min_ring=871ms | access: on under=0 min_ring=902ms | spindown: off under=0 min_ring=-
[stats] sd: chunks=4120 read_err=0 recoveries=0 failed_rec=0 max_read=2210us | stress: ON transitions=812 failures=0 | supercap=4148mV flt=1
```

- `under` counts samples the mixer had to fill with silence because the ring
  ran dry. **It should stay at 0.**
- `min_ring` is the lowest buffer level seen while streaming: the headroom
  against SD stalls.
- `max_read` is the slowest single 2 KB SD read since the last stats line.
- `i2s_underflow` means the audio core itself fell behind. It should never
  happen.

## Hardware test plan

Run these in order and send me the serial log from each.

1. **Boot and listen.** Power from USB with `"POWER_DETECTION": false`.
   Expect `[boot]` lines, spinup, then `[run] idle/access looping`. Press
   `a` for simulated activity and check the access overlay and RGB LED.
2. **Steady state, 10 minutes.** `under=0`, `i2s_underflow=0`,
   `read_err=0`. Note the `min_ring` and `max_read` values.
3. **Stress test, 10+ minutes.** Press `t`. This repeats the exact operation
   that froze CircuitPython: restarting SD-streamed voices while others play,
   several times a second. Expect thousands of transitions, `failures=0`,
   `under=0` and no freeze. It sounds choppy by design.
4. **SD abuse.** During the stress test, briefly pull the SD card and
   reinsert it. Expect `read_err` and `recoveries` to increase and playback
   to resume, with no hang. If it can't recover, the watchdog should reboot
   the board rather than freeze.
5. **Simulated power loss.** Press `p`. Spindown should play fully, with
   `[trace] spindown` lines every 100 ms, then the amp switches off.
6. **Real power cuts** (in the PC, or with power detection enabled), 20+
   cycles, including some within 30 s of boot (a partly charged supercap).
   Every cut should produce a spindown. The `[trace]` lines give the
   supercap curve during spinup and spindown, which is the data for the
   earlier hardware questions.
7. **Each sample pack**, especially `PB_MicroScience612`. Its 345 KB
   spindown may not fit in RAM: check whether the log says "spindown in RAM"
   or "will stream from SD", and that both work.

`WAIT_FOR_SERIAL_MS=1500` in `platformio.ini` delays boot by up to 1.5 s so
the console can attach. Remove it for normal use.
