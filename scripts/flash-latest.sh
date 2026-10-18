#!/usr/bin/env bash
# Flash an HDD Synth with release firmware from GitHub.
#
#   scripts/flash-latest.sh            # the latest release
#   scripts/flash-latest.sh v0.14.0    # a specific release
#
# Needs the GitHub CLI (`gh`). The board must be in bootloader mode, showing
# as a drive called RP2350: a blank board already is; otherwise hold BOOT
# (S2), tap RESET (S1) and let go of BOOT. The script waits for the drive.
#
# HDDSYNTH_DRIVE overrides where the drive is mounted (default
# /Volumes/RP2350 on macOS, /media/$USER/RP2350 on Linux).
set -euo pipefail

REPO="MaverickUK/HDDSynth_Native"
TAG="${1:-}"
WAIT_FOR_DRIVE_S=120
WAIT_FOR_BOOT_S=20

if [[ -n "${HDDSYNTH_DRIVE:-}" ]]; then
    DRIVE="$HDDSYNTH_DRIVE"
elif [[ "$(uname)" == "Darwin" ]]; then
    DRIVE="/Volumes/RP2350"
else
    DRIVE="/media/${USER}/RP2350"
fi

command -v gh >/dev/null || { echo "Needs the GitHub CLI: https://cli.github.com (brew install gh)" >&2; exit 1; }

# 1. Download the UF2.
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
echo "Downloading ${TAG:-the latest release} from $REPO..."
gh release download ${TAG:+"$TAG"} --repo "$REPO" --pattern 'hddsynth-mk5-*.uf2' --dir "$WORK"
UF2="$(ls "$WORK"/hddsynth-mk5-*.uf2 | grep -v -- '-stream-test' | head -1)"
[[ -f "$UF2" ]] || { echo "No firmware UF2 in that release." >&2; exit 1; }
echo "Got $(basename "$UF2")."

# 2. Wait for the board's bootloader drive.
if [[ ! -d "$DRIVE" ]]; then
    echo
    echo "Waiting for the RP2350 drive ($DRIVE)."
    echo "Put the board in bootloader mode: hold BOOT (S2), tap RESET (S1), let go of BOOT."
    for ((i = 0; i < WAIT_FOR_DRIVE_S; i++)); do
        [[ -d "$DRIVE" ]] && break
        sleep 1
    done
    [[ -d "$DRIVE" ]] || { echo "No RP2350 drive after ${WAIT_FOR_DRIVE_S} s; nothing flashed." >&2; exit 1; }
fi

# 3. Copy it on. The board flashes itself and reboots as soon as the copy
#    completes, so the drive disappears (macOS may say it wasn't ejected
#    properly; that's expected and harmless).
echo "Flashing..."
cp "$UF2" "$DRIVE/" 2>/dev/null || true  # the drive can vanish before cp finishes closing the file
sync

# 4. Check it came back as an HDD Synth (macOS only: uses system_profiler).
if [[ "$(uname)" == "Darwin" && -z "${HDDSYNTH_DRIVE:-}" ]]; then
    for ((i = 0; i < WAIT_FOR_BOOT_S; i++)); do
        if system_profiler SPUSBDataType 2>/dev/null | grep -q "HDDSYNTH"; then
            echo "Done: the board is running $(basename "$UF2" .uf2 | sed 's/hddsynth-mk5-//')."
            exit 0
        fi
        sleep 1
    done
    echo "Copied, but the board hasn't reappeared as HDDSYNTH within ${WAIT_FOR_BOOT_S} s." >&2
    echo "Check it's powered and its SD card is in; a board without a card halts after a beep." >&2
    exit 1
fi
echo "Done: copied $(basename "$UF2") to $DRIVE."
