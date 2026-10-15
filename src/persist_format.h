// What the firmware remembers across power cycles, and its byte format in
// flash. Pure C++ (no Arduino), host-tested.
//
// The CircuitPython firmware kept these in microcontroller.nvm (see
// settings.py NVM_ADDRESS_*). Here they live in one small record with a
// magic number, a format version and a CRC, so blank flash (all 0xFF), an
// older format or a corrupt record all fall back to defaults.
#pragma once

#include <stddef.h>
#include <stdint.h>

struct PersistedState {
    char desiredPack[64] = {0};  // "" = none chosen yet (use SAMPLE_PACK, else the first pack)
    bool jinglePlayed = false;
};

namespace persistfmt {

constexpr size_t RECORD_BYTES = 80;  // header 8 + payload 65 + padding + crc 4

// Write `s` into `out` (RECORD_BYTES).
void encode(const PersistedState& s, uint8_t* out);

// Read a record. Returns false (and leaves `s` at defaults) if it isn't a
// valid record of this format.
bool decode(const uint8_t* in, PersistedState& s);

} // namespace persistfmt
