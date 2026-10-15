// Persisted state in flash (Arduino-Pico EEPROM emulation: the last 4 KB
// flash sector). Core 0 only.
//
// Writing flash pauses core 1 and disables interrupts for ~50 ms (sector
// erase + program), so commit() must only be called with the audio stopped:
// the caller mutes the amp and keeps the voices idle. See CLAUDE.md.
#pragma once

#include "persist_format.h"

namespace persist {

// Load the record; invalid or blank flash gives defaults.
void begin();

// The live copy. Change it, then commit().
PersistedState& state();

// Write the record if it changed. Audio must be stopped (see above).
// Returns the time it took in ms, for the log.
uint32_t commit();

} // namespace persist
