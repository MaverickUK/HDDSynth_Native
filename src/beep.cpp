#include "beep.h"

#include <math.h>

namespace {

constexpr int SINE_BITS = 9;  // 512-entry table, as beep.py's lookup table
constexpr int SINE_SIZE = 1 << SINE_BITS;
int16_t sineTable[SINE_SIZE];

constexpr uint32_t ATTACK_MS = 20;
constexpr uint32_t DECAY_MS = 100;
constexpr uint32_t RELEASE_MS = 50;
constexpr float SUSTAIN = 0.8f;

uint32_t msToSamples(uint32_t ms, uint32_t rate) { return static_cast<uint32_t>((static_cast<uint64_t>(ms) * rate) / 1000); }

// Envelope while the note is held, `t` samples after it started (t may equal
// heldSamples, to get the level the release starts from).
float heldEnvelope(const BeepPattern& p, uint32_t t) {
    if (t < p.attackSamples) return static_cast<float>(t) / p.attackSamples;
    t -= p.attackSamples;
    if (t < p.decaySamples) return 1.0f - (1.0f - SUSTAIN) * static_cast<float>(t) / p.decaySamples;
    return SUSTAIN;
}

float envelope(const BeepPattern& p, uint32_t t) {
    if (t < p.heldSamples) return heldEnvelope(p, t);
    const uint32_t r = t - p.heldSamples;
    if (r >= p.releaseSamples) return 0.0f;
    return heldEnvelope(p, p.heldSamples) * (1.0f - static_cast<float>(r) / p.releaseSamples);
}

int32_t waveSample(BeepWave wave, uint32_t phase) {
    if (wave == BeepWave::Sine) return sineTable[phase >> (32 - SINE_BITS)];
    // Triangle as beep.py's table: rises from -32767 to +32767 over the first
    // half of the cycle, then falls back.
    const int32_t p = static_cast<int32_t>(phase >> 16);  // 0..65535
    return p < 32768 ? -32767 + p * 2 : 32767 - (p - 32768) * 2;
}

} // namespace

BeepPattern makeBeep(uint32_t sampleRate, uint32_t pitchHz, uint16_t count, uint32_t lengthMs, BeepWave wave) {
    BeepPattern p;
    p.phaseStep = static_cast<uint32_t>((static_cast<uint64_t>(pitchHz) << 32) / sampleRate);
    p.heldSamples = msToSamples(lengthMs, sampleRate);
    p.attackSamples = msToSamples(ATTACK_MS, sampleRate);
    p.decaySamples = msToSamples(DECAY_MS, sampleRate);
    p.releaseSamples = msToSamples(RELEASE_MS, sampleRate);
    const uint32_t buffer = msToSamples(BEEP_RELEASE_BUFFER_MS, sampleRate);
    const uint32_t gap = msToSamples(lengthMs * 4 / 10, sampleRate);
    p.periodSamples = p.heldSamples + buffer + gap;
    p.totalSamples = count ? count * (p.heldSamples + buffer) + (count - 1u) * gap : 0;
    p.wave = wave;
    return p;
}

void beepInit() {
    for (int i = 0; i < SINE_SIZE; ++i) {
        sineTable[i] = static_cast<int16_t>(lroundf(sinf(6.2831853f * i / SINE_SIZE) * 32767.0f));
    }
}

int16_t beepSample(const BeepPattern& p, uint32_t n) {
    if (n >= p.totalSamples || p.periodSamples == 0) return 0;
    const uint32_t t = n % p.periodSamples;  // position within this note
    const float env = envelope(p, t);
    if (env <= 0.0f) return 0;
    // Each note starts at phase 0, like a new synthio note.
    return static_cast<int16_t>(waveSample(p.wave, t * p.phaseStep) * env);
}

bool beepNoteHeld(const BeepPattern& p, uint32_t n) {
    return n < p.totalSamples && p.periodSamples && (n % p.periodSamples) < p.heldSamples;
}
