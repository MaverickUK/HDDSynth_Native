#include "audio_engine.h"

#include <Arduino.h>
#include <I2S.h>

#include "config.h"

namespace {

I2S i2s(OUTPUT);
Voice voices[VOICE_COUNT];

std::atomic<uint32_t> heartbeatCount{0};
std::atomic<uint32_t> worstMixUs{0};
std::atomic<bool> underflowLatched{false};

constexpr uint32_t STOP_TIMEOUT_MS = 100;  // core 1 acks within one 8 ms block

} // namespace

namespace audio {

void core1Setup() {
    i2s.setBCLK(cfg::PIN_I2S_BCLK);  // LRCLK = BCLK + 1
    i2s.setDATA(cfg::PIN_I2S_DATA);
    i2s.setBitsPerSample(16);
    // One I2S buffer word = one stereo frame (16-bit L + 16-bit R).
    i2s.setBuffers(cfg::I2S_BUFFERS, cfg::MIX_BLOCK_FRAMES);
    // begin() on core 1 so the DMA interrupt is serviced here, away from SD/USB work.
    i2s.begin(cfg::SAMPLE_RATE);
}

void core1Loop() {
    static int16_t mono[cfg::MIX_BLOCK_FRAMES];
    static uint32_t stereo[cfg::MIX_BLOCK_FRAMES];
    constexpr size_t blockBytes = cfg::MIX_BLOCK_FRAMES * sizeof(uint32_t);

    if (i2s.availableForWrite() < static_cast<int>(blockBytes)) {
        if (i2s.getUnderflow()) underflowLatched.store(true, std::memory_order_relaxed);
        return;  // loop1() is called again immediately
    }

    const uint32_t t0 = micros();
    mixBlock(voices, VOICE_COUNT, mono, cfg::MIX_BLOCK_FRAMES);
    for (size_t n = 0; n < cfg::MIX_BLOCK_FRAMES; ++n) {
        // MAX98357A plays the left channel (SD_MODE driven high); send the
        // same sample on both so any SD_MODE setting works.
        const uint16_t s = static_cast<uint16_t>(mono[n]);
        stereo[n] = (uint32_t(s) << 16) | s;
    }
    const uint32_t dt = micros() - t0;
    if (dt > worstMixUs.load(std::memory_order_relaxed)) worstMixUs.store(dt, std::memory_order_relaxed);

    i2s.write(reinterpret_cast<const uint8_t*>(stereo), blockBytes);
    heartbeatCount.fetch_add(1, std::memory_order_relaxed);
}

Voice& voice(VoiceId id) { return voices[id]; }

bool isPlaying(VoiceId id) {
    return voices[id].state.load(std::memory_order_acquire) == VS_PLAYING;
}

bool isIdle(VoiceId id) {
    return voices[id].state.load(std::memory_order_acquire) == VS_IDLE;
}

bool stop(VoiceId id) {
    Voice& v = voices[id];
    uint8_t expected = VS_PLAYING;
    v.state.compare_exchange_strong(expected, VS_STOP_REQ, std::memory_order_acq_rel);
    const uint32_t start = millis();
    while (v.state.load(std::memory_order_acquire) != VS_IDLE) {
        if (millis() - start > STOP_TIMEOUT_MS) return false;  // core 1 is stuck: watchdog will act
        tight_loop_contents();
    }
    return true;
}

static bool begin(Voice& v, uint16_t level) {
    // Statistics are deliberately not reset here, so they accumulate across
    // restarts (the stress test relies on that); startStress() clears them.
    v.targetLevel.store(level, std::memory_order_relaxed);
    v.playGen.fetch_add(1, std::memory_order_relaxed);
    v.state.store(VS_PLAYING, std::memory_order_release);
    return true;
}

bool startMemory(VoiceId id, const int16_t* data, uint32_t samples, bool loop, uint16_t level) {
    if (!stop(id)) return false;
    Voice& v = voices[id];
    v.mem = data;
    v.memLen = samples;
    v.ring = nullptr;
    v.loop = loop;
    v.eof.store(false, std::memory_order_relaxed);
    return begin(v, level);
}

bool startStream(VoiceId id, SampleRing* ring, uint16_t level) {
    // Caller primed the ring while the voice was idle; just hand it over.
    if (!isIdle(id)) return false;
    Voice& v = voices[id];
    v.mem = nullptr;
    v.memLen = 0;
    v.ring = ring;
    v.loop = false;
    return begin(v, level);
}

void setLevel(VoiceId id, uint16_t level) {
    voices[id].targetLevel.store(level, std::memory_order_relaxed);
}

void markEndOfStream(VoiceId id) {
    voices[id].eof.store(true, std::memory_order_release);
}

uint32_t heartbeat() { return heartbeatCount.load(std::memory_order_relaxed); }

uint32_t maxMixMicros() { return worstMixUs.exchange(0, std::memory_order_relaxed); }

bool i2sUnderflowed() { return underflowLatched.exchange(false, std::memory_order_relaxed); }

} // namespace audio
