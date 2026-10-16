#include "levels_format.h"

#include <string.h>

#include "crc32.h"

namespace levels {

namespace {

constexpr uint32_t MAGIC = 0x56444448;  // "HDDV" little-endian
constexpr uint16_t VERSION = 1;

// Layout (little-endian): 0 magic, 4 version (u16), 6 reserved (u16),
// 8 seq (u32), 12 volumePct, 13 balancePct, 14-15 reserved, 16 crc32 of 0-15.
constexpr size_t CRC_AT = 16;

} // namespace

void encode(const Record& r, uint8_t* sector) {
    memset(sector, 0, SECTOR_BYTES);
    memcpy(sector, &MAGIC, 4);
    memcpy(sector + 4, &VERSION, 2);
    memcpy(sector + 8, &r.seq, 4);
    sector[12] = r.volumePct;
    sector[13] = r.balancePct;
    const uint32_t crc = crc32(sector, CRC_AT);
    memcpy(sector + CRC_AT, &crc, 4);
}

bool decode(const uint8_t* sector, Record& r) {
    uint32_t magic, crc;
    uint16_t version;
    memcpy(&magic, sector, 4);
    memcpy(&version, sector + 4, 2);
    memcpy(&crc, sector + CRC_AT, 4);
    if (magic != MAGIC || version != VERSION || crc != crc32(sector, CRC_AT)) return false;
    memcpy(&r.seq, sector + 8, 4);
    r.volumePct = sector[12] <= 100 ? sector[12] : NOT_SAVED;
    r.balancePct = sector[13] <= 100 ? sector[13] : NOT_SAVED;
    return true;
}

bool newest(const uint8_t* slotA, const uint8_t* slotB, Record& r) {
    Record a, b;
    const bool okA = decode(slotA, a), okB = decode(slotB, b);
    if (!okA && !okB) return false;
    // Sequence numbers only go up; compare with wraparound in mind.
    r = !okB || (okA && static_cast<int32_t>(a.seq - b.seq) > 0) ? a : b;
    return true;
}

} // namespace levels
