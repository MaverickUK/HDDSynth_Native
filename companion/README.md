# HDD Synth Companion

A small tray app (menu bar app on macOS) for the
[HDD Synth](https://github.com/MaverickUK/HDDSynth). Plug the HDD Synth into
your Mac or PC with a USB cable and it plays **that computer's disk
activity**: no HDD LED header or wiring needed. From the tray icon you can
also choose the sample pack and set the volume and balance.

It needs HDD Synth firmware **v0.19.0** or later.

## Install

It's a Python app, installed with [pipx](https://pipx.pypa.io). pipx
installs it into its own folder, and you never download an app bundle, so
there are no Gatekeeper or SmartScreen warnings to click through, and no
admin rights or drivers are needed.

### macOS

1. Open Terminal and install pipx, using [Homebrew](https://brew.sh):

   ```sh
   brew install pipx
   pipx ensurepath
   ```

   Without Homebrew, use `python3 -m pip install --user pipx` then
   `python3 -m pipx ensurepath`. If macOS offers to install the developer
   tools for `python3`, say yes.
2. Close and reopen Terminal, then install the app:

   ```sh
   pipx install "git+https://github.com/MaverickUK/HDDSynth_Native.git#subdirectory=companion"
   ```
3. Start it with `hddsynth-companion`. A drive icon appears in the menu bar.

### Windows 10 and 11

1. Install Python 3 from [python.org](https://www.python.org/downloads/).
   Tick **"Add python.exe to PATH"** in the installer.
2. Open a Command Prompt and install pipx:

   ```bat
   py -m pip install --user pipx
   py -m pipx ensurepath
   ```
3. Close and reopen the Command Prompt, then install the app:

   ```bat
   pipx install "git+https://github.com/MaverickUK/HDDSynth_Native.git#subdirectory=companion"
   ```
4. Start it with `hddsynth-companion`. A drive icon appears in the tray
   (it may hide under the **^** arrow; drag it out to keep it in view).

Windows needs no driver: the HDD Synth is a standard USB serial device.

### Updating and removing

```sh
pipx upgrade hddsynth-companion
pipx uninstall hddsynth-companion
```

Turn off **Start at login** before uninstalling.

## Using it

Plug in the HDD Synth. The app finds it by itself within a couple of
seconds, and again after it's unplugged or restarted.

The tray icon shows the state:

| Icon | Meaning |
|---|---|
| Grey drive | Not connected. |
| Dark LED | Connected, the disk is quiet. |
| Orange LED | Disk activity is being sent. |

The **tray menu** has:
- The connection status and the current pack.
- **Sample pack**: switch pack, just like the action button. It's
  remembered.
- **Settings…** (on Windows a left click on the icon opens it too).
- **Start at login**.
- **About HDD Synth Companion…**: the app's version, the OS, and the
  connected HDD Synth's firmware.
- **Quit**.

The **settings** window has:
- **Supercap**: how charged the HDD Synth's supercap is, and so whether it
  could play a spindown if the power were cut:
  - **red**: not enough yet;
  - **orange**: enough for a typical spindown;
  - **green**: enough for any pack's.

  The white mark is the typical spindown's needs. These thresholds are
  estimates until a real power cut is measured. It fills up over the first
  half minute or so after power-on.
- **Sample pack**, **Volume** and **Balance**: the same settings as the
  encoder. They're saved on the device (if `SAVE_VOLUME_BALANCE` is on), and
  follow the encoder if you turn it. Balance mixes the idle sound (left)
  against the access sound (right).
- **Play this computer's disk activity**: untick it to use the app only as a
  remote control.
- **Sensitivity** and the **disk activity** meter, described next.
- **Disks**: which disks count. By default all of them; untick, say, a
  backup drive that's always busy.
- **Port**: "Automatic" unless the app can't find the HDD Synth by itself.

### Tuning the sensitivity

A modern computer reads and writes its SSD a little all the time. With the
sensitivity too high, the access sound never stops.

The bar under the sensitivity slider shows how busy the disk is. The red
line is the threshold, right under the slider's handle; activity past it
turns the bar orange and plays the access sound. Move the slider until the
bar stays left of the line while the computer is idle, but crosses it when
you open an app or copy a file. The default threshold is about 0.4 MB/s; at
the "more sensitive" end, any disk access at all counts.

Large file copies are detected as **sequential** transfers. With
`EXPERIMENTAL_HDDSYNTHPRO_SUPPORT` on in the device's settings.json, a pack's
`sequential.wav` plays for them.

## Troubleshooting

- **"Not connected" with the HDD Synth plugged in:**
  - Use a data USB cable, not a charge-only one.
  - The HDD Synth needs firmware v0.19.0 or later: the app checks each
    Raspberry Pi USB serial port by asking for its status. The tray icon's
    tooltip says when a port didn't answer.
  - Check that nothing else has its serial port open: the Arduino IDE's
    serial monitor, PlatformIO, or `screen`.
  - Only one program can use the port at a time. While the app runs, the
    HDD Synth's serial console isn't available to a terminal: quit the app
    first.
- **No tray icon on Windows:** look under the **^** arrow in the taskbar.
- **The access sound plays non-stop:** lower the sensitivity, or untick
  disks that are always busy.

The app keeps its own settings in `config.json` in
`~/Library/Application Support/hddsynth-companion` (macOS) or
`%LOCALAPPDATA%\hddsynth-companion` (Windows).

## How it works

Every 20 ms the app reads the operating system's disk counters (IOKit on
macOS, the disk performance counters on Windows, both through
[psutil](https://github.com/giampaolo/psutil), without admin rights). It
sends the result to the HDD Synth as a line of text over USB serial:
`$HDA,1,1,S,R*4C` means busy, sequential, reading. It sends one on every
change and every 200 ms, and the device treats the app as gone after a
second of silence. Queries and commands (`$HDQ`, `$HDC`) and the device's
replies (`$HDS`, `$HDP`, `$HDK`) use the same format. The HDD Synth
firmware's README has the full protocol table.

## Development

```sh
cd companion
python3 -m venv .venv && .venv/bin/pip install -e ".[test]"
.venv/bin/pytest          # protocol, activity detection, port matching (no GUI needed)
.venv/bin/hddsynth-companion
```

The protocol tests use the same sentences as the firmware's host tests
(`test/native/test_main.cpp`), so the two sides stay in step.
