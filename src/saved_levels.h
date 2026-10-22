// SAVE_VOLUME_BALANCE: remembers the volume and balance after they are
// changed by hand, on the SD card (/state.bin; see levels_format.h for why
// not flash). Core 0 only.
#pragma once

#include "settings.h"

namespace savedlevels {

// At boot, after the card is mounted and settings.json is read. If saving
// is on, applies saved values that settings.json doesn't override, and logs
// what it did.
void begin(Settings& s);

// At boot: did a saved balance get applied (so a pack's own default yields)?
bool balanceRestored();

// Call every loop pass while running: saves 2 s after the last change.
void service();

// Factory reset: forget the saved values.
void clear();

} // namespace savedlevels
