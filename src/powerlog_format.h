// On-card format of the power log: one 512-byte SD sector at a time.
//
// The log has to survive the board losing power, so it is written with raw
// sector writes into a preallocated file (no filesystem metadata changes
// while the supply is failing). Each sector carries the session number, its
// index in the session and a CRC, so a reader can find where the latest
// session ends and spot a sector torn by power dying mid-write.
//
// Pure C++ (no Arduino), so it is covered by the host-side tests.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace powerlog {

constexpr size_t SECTOR_BYTES = 512;
constexpr size_t HEADER_BYTES = 16;
constexpr size_t TEXT_BYTES = SECTOR_BYTES - HEADER_BYTES;

// Fill `out` (SECTOR_BYTES) with a header and `len` bytes of text (clamped
// to TEXT_BYTES); the rest is zeroed.
void encodeSector(uint8_t* out, uint32_t session, uint16_t index, const char* text, size_t len);

// Check a sector. On success sets session, index and text (pointing into
// `in`, not NUL-terminated) and returns the text length; returns -1 if the
// sector is blank, from another format, or fails its CRC.
int decodeSector(const uint8_t* in, uint32_t& session, uint16_t& index, const char*& text);

uint32_t crc32(const uint8_t* data, size_t len);

} // namespace powerlog
