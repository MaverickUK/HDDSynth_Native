// Standard CRC-32 (as zlib), bit by bit: small, and speed is irrelevant for
// the few hundred bytes it checks. Pure C++, host-tested.
#pragma once

#include <stddef.h>
#include <stdint.h>

// Continue a CRC over more data: start with 0xFFFFFFFF, finish with ~.
inline uint32_t crc32Update(uint32_t c, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        c ^= data[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c;
}

inline uint32_t crc32(const uint8_t* data, size_t len) { return ~crc32Update(0xFFFFFFFFu, data, len); }
