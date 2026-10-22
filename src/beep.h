// Beep synthesis. Pure C++ (no Arduino), host-tested.
//
// A beep pattern (pitch, waveform, note length, count) is synthesised by the
// mixer on core 1, one sample at a time from its position in the pattern, so
// it needs no sample buffer and never blocks. It reproduces the CircuitPython
// beep.py: a synthio note with an ADSR envelope (attack 20 ms, decay 100 ms
// to 0.8, release 50 ms), held for `length`, then a 50 ms release buffer and
// a gap of 0.4 x length before the next note.
#pragma once

#include <stdint.h>

enum class BeepWave : uint8_t { Sine, Triangle };

struct BeepPattern {
    uint32_t phaseStep = 0;      // Q32 phase increment per sample (the pitch)
    uint32_t heldSamples = 0;    // note on (LENGTH)
    uint32_t attackSamples = 0;
    uint32_t decaySamples = 0;
    uint32_t releaseSamples = 0;
    uint32_t periodSamples = 0;  // from one note's start to the next
    uint32_t totalSamples = 0;   // whole pattern, last note's release included
    BeepWave wave = BeepWave::Sine;
};

constexpr uint32_t BEEP_RELEASE_BUFFER_MS = 50;  // beep.RELEASE_BUFFER_S

BeepPattern makeBeep(uint32_t sampleRate, uint32_t pitchHz, uint16_t count, uint32_t lengthMs, BeepWave wave);

// Fill the sine table. Call once before the first beepSample().
void beepInit();

// Sample `n` of the pattern at full scale (the voice level scales it); 0
// once the pattern has ended.
int16_t beepSample(const BeepPattern& p, uint32_t n);

// The level a beep plays at, for a given user volume (0..1): it follows the
// volume, so a beep is never much louder than the drive sound, but never
// drops below 0.25 (still audible) or above 0.8 (the CircuitPython level).
inline float beepLevelFor(float volume) {
    return volume < 0.25f ? 0.25f : (volume > 0.8f ? 0.8f : volume);
}

// For the RGB LED: is a note held (not releasing or in a gap) at sample `n`?
bool beepNoteHeld(const BeepPattern& p, uint32_t n);
