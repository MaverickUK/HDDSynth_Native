// Voices and the mixer. Platform independent (unit tested natively).
//
// Ownership protocol between the two cores, per voice:
//
//   IDLE        core 0 owns the voice: it may set the source, reset/fill the
//               ring and change `loop`. Core 1 does not touch the voice.
//   PLAYING     core 1 reads the source. Core 0 may only push into the ring,
//               set `eof` and change `targetLevel`.
//   STOP_REQ    core 0 asked core 1 to stop; core 1 moves it to IDLE at the
//               start of its next block (<= one block, 8 ms).
//
// Core 1 also moves PLAYING -> IDLE itself when a source ends. Every handover
// is a release-store / acquire-load on `state`, so data written before the
// handover is visible after it. Nothing in here blocks, allocates or touches
// the SD card.
#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "beep.h"
#include "config.h"
#include "ring_buffer.h"

using SampleRing = SpscRing<int16_t, cfg::RING_SAMPLES>;

enum VoiceState : uint8_t { VS_IDLE = 0, VS_PLAYING = 1, VS_STOP_REQ = 2 };

constexpr uint16_t LEVEL_UNITY = 32768;  // Q15 gain of 1.0

struct Voice {
    // --- handover / shared ---
    std::atomic<uint8_t> state{VS_IDLE};
    std::atomic<uint32_t> playGen{0};       // bumped by core 0 on every start
    std::atomic<uint16_t> targetLevel{0};   // Q15, may change any time
    std::atomic<bool> eof{false};           // stream mode: producer has pushed the last sample

    // --- written by core 0 only while IDLE ---
    const int16_t* mem = nullptr;           // memory mode source (nullptr = stream mode)
    uint32_t memLen = 0;
    SampleRing* ring = nullptr;             // stream mode source
    const BeepPattern* beep = nullptr;      // beep mode: synthesised here (takes priority)
    bool loop = false;                      // memory mode only; streams loop in the producer

    // --- owned by core 1 ---
    uint32_t seenGen = 0;
    uint32_t memPos = 0;                    // memory and beep modes: samples played
    uint16_t curLevel = 0;

    // --- statistics (written by core 1, read by core 0) ---
    std::atomic<uint32_t> underrunSamples{0};
    std::atomic<uint32_t> minRingFill{0xFFFFFFFF};  // lowest ring fill seen while playing
};

// Mix `frames` mono frames from `voices` into `out` (saturated int16).
// Called on core 1 only.
void mixBlock(Voice* voices, size_t voiceCount, int16_t* out, size_t frames);

// Convert a 0.0..1.0 gain to Q15.
inline uint16_t levelFromFloat(float g) {
    if (g <= 0.0f) return 0;
    if (g >= 1.0f) return LEVEL_UNITY;
    return static_cast<uint16_t>(g * LEVEL_UNITY + 0.5f);
}
