// USB drive mode: hold the action button while powering on, and the board
// shares its SD card with the computer as a USB drive, so packs and
// settings.json can be edited without removing the card. No audio plays.
// Ejecting the drive on the computer (or pressing the action button) reboots
// into normal use. Core 0 only; nothing else touches the card meanwhile.
#pragma once

namespace usbdrive {

// Is the action button held (checked at boot, for ~300 ms)?
bool requested();

// Share the card until it's ejected or the action button is pressed, then
// reboot. Never returns.
[[noreturn]] void run();

} // namespace usbdrive
