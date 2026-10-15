#include "powerlog_format.h"

#include <string.h>

namespace powerlog {

namespace {

constexpr uint32_t MAGIC = 0x474C5048;  // "HPLG" little-endian

// Header layout (little-endian, as the RP2350 and every host we test on are):
//   0  magic    4  session    8  index (u16)   10 length (u16)   12 crc32
// The CRC covers bytes 0-11 and the text.
void put32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
void put16(uint8_t* p, uint16_t v) { memcpy(p, &v, 2); }
uint32_t get32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
uint16_t get16(const uint8_t* p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

// Standard CRC-32 (as zlib), bit by bit: small, and speed is irrelevant here.
uint32_t crcUpdate(uint32_t c, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        c ^= data[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c;
}

// CRC of the first 12 header bytes followed by the text.
uint32_t headerAndTextCrc(const uint8_t* sector, size_t len) {
    const uint32_t c = crcUpdate(0xFFFFFFFFu, sector, 12);
    return ~crcUpdate(c, sector + HEADER_BYTES, len);
}

} // namespace

uint32_t crc32(const uint8_t* data, size_t len) { return ~crcUpdate(0xFFFFFFFFu, data, len); }

void encodeSector(uint8_t* out, uint32_t session, uint16_t index, const char* text, size_t len) {
    if (len > TEXT_BYTES) len = TEXT_BYTES;
    memset(out, 0, SECTOR_BYTES);
    put32(out, MAGIC);
    put32(out + 4, session);
    put16(out + 8, index);
    put16(out + 10, static_cast<uint16_t>(len));
    memcpy(out + HEADER_BYTES, text, len);
    put32(out + 12, headerAndTextCrc(out, len));
}

int decodeSector(const uint8_t* in, uint32_t& session, uint16_t& index, const char*& text) {
    if (get32(in) != MAGIC) return -1;
    const uint16_t len = get16(in + 10);
    if (len > TEXT_BYTES) return -1;
    if (get32(in + 12) != headerAndTextCrc(in, len)) return -1;
    session = get32(in + 4);
    index = get16(in + 8);
    text = reinterpret_cast<const char*>(in + HEADER_BYTES);
    return len;
}

} // namespace powerlog
