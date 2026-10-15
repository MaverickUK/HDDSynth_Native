#include "persist_format.h"

#include <string.h>

#include "crc32.h"

namespace persistfmt {

namespace {

constexpr uint32_t MAGIC = 0x53444448;  // "HDDS" little-endian
constexpr uint16_t VERSION = 1;

// Layout (little-endian):
//   0  magic (u32)   4  version (u16)   6  payload length (u16)
//   8  desiredPack (64 bytes, NUL-padded)   72 jinglePlayed (u8)
//   76 crc32 of bytes 0-75
constexpr size_t PACK_AT = 8;
constexpr size_t JINGLE_AT = 72;
constexpr size_t CRC_AT = 76;
constexpr uint16_t PAYLOAD_BYTES = JINGLE_AT + 1 - PACK_AT;

} // namespace

void encode(const PersistedState& s, uint8_t* out) {
    memset(out, 0, RECORD_BYTES);
    memcpy(out, &MAGIC, 4);
    memcpy(out + 4, &VERSION, 2);
    memcpy(out + 6, &PAYLOAD_BYTES, 2);
    strncpy(reinterpret_cast<char*>(out + PACK_AT), s.desiredPack, sizeof(s.desiredPack) - 1);
    out[JINGLE_AT] = s.jinglePlayed ? 1 : 0;
    const uint32_t crc = crc32(out, CRC_AT);
    memcpy(out + CRC_AT, &crc, 4);
}

bool decode(const uint8_t* in, PersistedState& s) {
    s = PersistedState{};
    uint32_t magic, crc;
    uint16_t version, payload;
    memcpy(&magic, in, 4);
    memcpy(&version, in + 4, 2);
    memcpy(&payload, in + 6, 2);
    memcpy(&crc, in + CRC_AT, 4);
    if (magic != MAGIC || version != VERSION || payload != PAYLOAD_BYTES || crc != crc32(in, CRC_AT)) {
        return false;
    }
    memcpy(s.desiredPack, in + PACK_AT, sizeof(s.desiredPack) - 1);
    s.desiredPack[sizeof(s.desiredPack) - 1] = 0;
    s.jinglePlayed = in[JINGLE_AT] != 0;
    return true;
}

} // namespace persistfmt
