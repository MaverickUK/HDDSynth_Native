#include "wav.h"

#include <string.h>

namespace {

uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

constexpr uint16_t WAVE_FORMAT_PCM = 0x0001;
constexpr uint16_t WAVE_FORMAT_EXTENSIBLE = 0xFFFE;

} // namespace

const char* wavErrorName(WavError e) {
    switch (e) {
        case WavError::Ok: return "ok";
        case WavError::NotRiff: return "not a RIFF/WAVE file";
        case WavError::NoFmt: return "missing fmt chunk";
        case WavError::NoData: return "missing data chunk";
        case WavError::Truncated: return "header truncated";
        case WavError::Unsupported: return "unsupported encoding";
    }
    return "?";
}

WavError parseWavHeader(const uint8_t* buf, size_t len, uint32_t fileSize, WavInfo& out) {
    out = WavInfo{};
    if (len < 12) return WavError::Truncated;
    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) return WavError::NotRiff;

    bool haveFmt = false;
    size_t pos = 12;
    while (pos + 8 <= len) {
        const uint8_t* chunk = buf + pos;
        const uint32_t chunkLen = rd32(chunk + 4);
        const size_t body = pos + 8;

        if (memcmp(chunk, "fmt ", 4) == 0) {
            if (chunkLen < 16) return WavError::Unsupported;
            if (body + 16 > len) return WavError::Truncated;
            uint16_t format = rd16(buf + body);
            if (format == WAVE_FORMAT_EXTENSIBLE) {
                // Subformat GUID starts 24 bytes into an 40-byte extensible fmt chunk;
                // its first two bytes are the actual format tag.
                if (chunkLen < 40) return WavError::Unsupported;
                if (body + 26 > len) return WavError::Truncated;
                format = rd16(buf + body + 24);
            }
            out.format = format;
            out.channels = rd16(buf + body + 2);
            out.sampleRate = rd32(buf + body + 4);
            out.bitsPerSample = rd16(buf + body + 14);
            haveFmt = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            if (!haveFmt) return WavError::NoFmt;
            if (out.format != WAVE_FORMAT_PCM) return WavError::Unsupported;
            out.dataOffset = static_cast<uint32_t>(body);
            uint32_t avail = fileSize > out.dataOffset ? fileSize - out.dataOffset : 0;
            // Writers that stream sometimes leave 0 or 0xFFFFFFFF here; trust the file size then.
            uint32_t bytes = (chunkLen == 0 || chunkLen > avail) ? avail : chunkLen;
            // Never split a sample frame.
            const uint32_t frame = uint32_t(out.channels) * (out.bitsPerSample / 8);
            if (frame) bytes -= bytes % frame;
            out.dataBytes = bytes;
            return WavError::Ok;
        }

        // Chunks are word aligned: odd lengths carry one pad byte.
        pos = body + chunkLen + (chunkLen & 1);
    }
    // Ran out of buffer: if it held the whole file there really is no data chunk.
    if (len >= fileSize) return haveFmt ? WavError::NoData : WavError::NoFmt;
    return WavError::Truncated;
}

bool wavRateSupported(uint32_t rate) {
    for (uint32_t r : WAV_SUPPORTED_RATES)
        if (r == rate) return true;
    return false;
}

bool wavIsPlayable(const WavInfo& w) {
    return w.format == WAVE_FORMAT_PCM && w.channels == 1 && w.bitsPerSample == 16 &&
           wavRateSupported(w.sampleRate) && w.dataBytes > 0;
}
