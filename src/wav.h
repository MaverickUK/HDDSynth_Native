// Minimal RIFF/WAVE header parser. Platform independent (unit tested natively).
#pragma once

#include <stddef.h>
#include <stdint.h>

struct WavInfo {
    uint16_t format = 0;          // 1 = PCM (WAVE_FORMAT_EXTENSIBLE with PCM subformat is mapped to 1)
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 0;
    uint32_t dataOffset = 0;      // byte offset of first sample in the file
    uint32_t dataBytes = 0;       // length of sample data in bytes (clamped to file size)
};

enum class WavError {
    Ok = 0,
    NotRiff,
    NoFmt,
    NoData,
    Truncated,
    Unsupported,
};

const char* wavErrorName(WavError e);

// Parse a header from the first `len` bytes of a file of total size `fileSize`.
// Walks chunks (so LIST/fact/bext chunks before 'data' are fine). `len` should
// cover everything up to the start of the 'data' payload; 4 KB is plenty for
// normal files.
WavError parseWavHeader(const uint8_t* buf, size_t len, uint32_t fileSize, WavInfo& out);

// Sample rates the firmware plays: the audio output switches to the pack's
// rate (all of a pack's WAVs must share one; see loadPack() in main.cpp).
constexpr uint32_t WAV_SUPPORTED_RATES[] = {16000, 22050, 32000, 44100, 48000};
bool wavRateSupported(uint32_t rate);

// True if the file matches what the mixer plays: PCM, mono, 16-bit, at a
// supported rate.
bool wavIsPlayable(const WavInfo& w);
