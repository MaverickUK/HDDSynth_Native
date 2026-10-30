// Audio engine: runs the mixer on core 1 and feeds I2S. Core 0 controls it
// through the functions below. None of them touch the SD card.
#pragma once

#include <stdint.h>

#include "mixer.h"

enum VoiceId : uint8_t {
    VOICE_SPINUP = 0,   // stream (boot only)
    VOICE_IDLE,         // stream, looping
    VOICE_ACCESS,       // stream, looping
    VOICE_SPINDOWN,     // memory if it fits in RAM, otherwise stream
    VOICE_BEEP,         // memory
    VOICE_SEQUENTIAL,   // stream, looping (sequential.wav, with EXPERIMENTAL_HDDSYNTHPRO_SUPPORT)
    VOICE_DRIVE,        // the modelled drive (a pack with drive.json): synthesised, never ends
    VOICE_COUNT
};

namespace audio {

// Core 1 entry points (called from setup1()/loop1()).
void core1Setup();
void core1Loop();

// ---- Core 0 control ----
// Start a voice reading from RAM. Stops the voice first if needed.
bool startMemory(VoiceId id, const int16_t* data, uint32_t samples, bool loop, uint16_t level);
// Start a voice reading from a ring the caller has already primed.
bool startStream(VoiceId id, SampleRing* ring, uint16_t level);
// Start the modelled drive's voice. Call `engine->begin()` before, while
// the voice is idle; from then on, control the engine through its own
// methods. The engine must stay valid until the voice is idle.
bool startDrive(VoiceId id, DriveEngine* engine, uint16_t level);
// Start a synthesised beep. `pattern` must stay valid until the voice is idle.
bool startBeep(VoiceId id, const BeepPattern* pattern, uint16_t level);
// Ask core 1 to stop a voice and wait (bounded) until it has. Returns false on timeout.
bool stop(VoiceId id);
bool isPlaying(VoiceId id);
bool isIdle(VoiceId id);
void setLevel(VoiceId id, uint16_t level);
void markEndOfStream(VoiceId id);
Voice& voice(VoiceId id);

// Output sample rate, set from the pack (all of its WAVs share one rate).
// Core 1 applies it before its next block; this waits (bounded) for that and
// returns false if it didn't happen within 100 ms. Change it only with
// nothing playing that's still at the old rate (beeps are made at whatever
// rate is current when they start).
bool setSampleRate(uint32_t hz);
uint32_t sampleRate();

// After the spindown, while waiting on the supercap: stop I2S and park core
// 1 asleep until the next reboot (there is no way back). Waits for core 1 to
// confirm; returns false if it didn't within 100 ms. Nothing may play after.
bool shutdownForLowPower();

// ---- Health ----
uint32_t heartbeat();          // increments once per mixed block
uint16_t outputPeak();         // peak |sample| (0..32767) of the latest mixed block
uint32_t maxMixMicros();       // worst block mix time since last call (resets)
bool i2sUnderflowed();         // I2S ran dry since last call (resets)

} // namespace audio
