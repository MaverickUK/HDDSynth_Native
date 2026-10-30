// USB drive mode: hold the action button while powering on, and the board
// shares its SD card with the computer as a USB drive, so packs and
// settings.json can be edited without removing the card. No audio plays.
// Ejecting the drive on the computer (or pressing the action button) reboots
// into normal use. Core 0 only; nothing else touches the card meanwhile.
#pragma once

namespace usbdrive {

// Should this boot share the card: was it asked for before the reboot
// (requestAtNextBoot()), or is the action button held (for ~300 ms)?
bool requested();

// Ask the next boot to share the card (the serial `u` command), then reboot.
// The request lives in a watchdog scratch register, which keeps its value
// through a reset but not a power cut, and is cleared once it's read.
void requestAtNextBoot();

// Share the card until it's ejected or the action button is pressed, then
// reboot. Never returns.
[[noreturn]] void run();

} // namespace usbdrive
