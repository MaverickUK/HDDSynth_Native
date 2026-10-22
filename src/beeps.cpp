#include "beeps.h"

#include <Arduino.h>

#include "audio_engine.h"
#include "beep.h"
#include "config.h"
#include "io.h"

namespace beeps {

namespace {

// beep_profiles.py
constexpr uint32_t PITCH_HIGH = 1200;
constexpr uint32_t PITCH_LOW = 400;
constexpr uint32_t LENGTH_SHORT_MS = 100;
constexpr uint32_t LENGTH_LONG_MS = 1000;
constexpr uint32_t ERROR_REPEAT_MS = 4000;  // RGB_LED_ERROR_REPEAT_S

struct Profile {
    uint32_t pitchHz;
    uint16_t count;
    uint32_t lengthMs;
    BeepWave wave;
    bool error;
};

Profile profileFor(Type t) {
    switch (t) {
        case Type::ChangePack:      return {PITCH_HIGH, 1, LENGTH_SHORT_MS, BeepWave::Triangle, false};
        case Type::FactoryReset:    return {PITCH_HIGH, 1, LENGTH_LONG_MS, BeepWave::Triangle, false};
        case Type::VolumeMode:      return {PITCH_HIGH, 2, LENGTH_SHORT_MS, BeepWave::Triangle, false};
        case Type::BalanceMode:     return {PITCH_HIGH, 3, LENGTH_SHORT_MS, BeepWave::Triangle, false};
        case Type::NoSdCard:        return {PITCH_LOW, 1, LENGTH_SHORT_MS, BeepWave::Sine, true};
        case Type::NoPacksOnSdCard: return {PITCH_LOW, 2, LENGTH_SHORT_MS, BeepWave::Sine, true};
        case Type::CorruptedPack:   return {PITCH_LOW, 4, LENGTH_SHORT_MS, BeepWave::Sine, true};
        case Type::PackNotFound:    return {PITCH_LOW, 5, LENGTH_SHORT_MS, BeepWave::Sine, true};
    }
    return {PITCH_LOW, 1, LENGTH_SHORT_MS, BeepWave::Sine, true};
}

BeepPattern makePattern(const Profile& p, uint32_t sampleRate) {
    return makeBeep(sampleRate, p.pitchHz, p.count, p.lengthMs, p.wave);
}

void colour(bool error) {
    if (error) io::rgb(255, 0, 0);  // RGB_LED_ERROR_COLOUR
    else io::rgb(0, 0, 255);        // RGB_LED_OPERATION_COLOUR
}

const Settings* settings = nullptr;  // for the user's volume; null before settings are read

// Read by core 1 while the beep voice plays; only rewritten after stopping it.
BeepPattern pattern;
uint32_t patternRate = cfg::DEFAULT_SAMPLE_RATE;  // the output rate the pattern was made for
bool playing = false;
bool error = false;
uint32_t startMs = 0;
bool ledLit = false;

uint32_t elapsedSamples() {
    return static_cast<uint32_t>(static_cast<uint64_t>(millis() - startMs) * patternRate / 1000);
}

} // namespace

void begin(const Settings& s) { settings = &s; }

void play(Type t) {
    const Profile p = profileFor(t);
    audio::stop(VOICE_BEEP);  // never rewrite the pattern under core 1
    patternRate = audio::sampleRate();  // made at the pack's rate, whatever it is
    pattern = makePattern(p, patternRate);
    error = p.error;
    startMs = millis();
    // Before settings exist (e.g. the no-SD-card beep), the old fixed level.
    const float level = settings ? beepLevelFor(settings->volume) : 0.8f;
    playing = audio::startBeep(VOICE_BEEP, &pattern, levelFromFloat(level));
    ledLit = false;
}

bool active() { return playing; }

bool service() {
    if (!playing) return false;
    const uint32_t n = elapsedSamples();
    // Done once the voice has played it out; the margin covers I2S latency,
    // and makes sure a wait can never hang on a voice that didn't finish.
    const uint32_t margin = patternRate / 2;
    if (n >= pattern.totalSamples && (audio::isIdle(VOICE_BEEP) || n >= pattern.totalSamples + margin)) {
        playing = false;
        io::rgbOff();
        ledLit = false;
        return true;
    }
    const bool held = beepNoteHeld(pattern, n);
    if (held != ledLit) {
        ledLit = held;
        if (held) colour(error);
        else io::rgbOff();
    }
    return false;
}

void waitDone(void (*idle)()) {
    while (playing) {
        service();
        if (idle) idle();
        delay(1);
    }
}

bool errorLedOn(Type t, uint32_t nowMs) {
    // Timing only, so lay the pattern out in 1 ms units (a "1000 Hz rate"):
    // a sample index is then simply a millisecond.
    const BeepPattern p = makePattern(profileFor(t), 1000);
    const uint32_t cycleMs = p.totalSamples + ERROR_REPEAT_MS;
    return beepNoteHeld(p, nowMs % cycleMs);
}

} // namespace beeps
