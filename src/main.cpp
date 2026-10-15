// HDD Synth MK5 firmware: proof of concept.
//
// Core 1: audio engine only (mixer -> I2S), reads RAM, never touches the SD card.
// Core 0: everything else, and the single owner of the SD card.
//
// Serial commands (115200, USB): h help, s stats, t stress test on/off,
// p simulate power loss, a toggle simulated activity, + / - volume,
// n next sample pack (for testing), l print the last power-loss log,
// r reboot, b reboot to USB bootloader.
#include <Arduino.h>
#include <hardware/structs/watchdog.h>

#include <initializer_list>

#include "audio_engine.h"
#include "config.h"
#include "io.h"
#include "power.h"
#include "powerlog.h"
#include "sd_streamer.h"
#include "settings.h"
#include "version.h"

#ifndef WAIT_FOR_SERIAL_MS
#define WAIT_FOR_SERIAL_MS 0
#endif

// Test builds only (env:mk5_stream_test): never load spindown into RAM, so
// the streamed spindown path runs with packs whose spindown would fit.
#ifndef FORCE_STREAMED_SPINDOWN
#define FORCE_STREAMED_SPINDOWN 0
#endif

// ---------------------------------------------------------------------------
// Core 1
// ---------------------------------------------------------------------------
void setup1() { audio::core1Setup(); }
void loop1() { audio::core1Loop(); }

// ---------------------------------------------------------------------------
// Core 0
// ---------------------------------------------------------------------------
namespace {

enum class State { Boot, Spinup, Run, Spindown, WaitPower, Halted };
const char* stateName(State s) {
    switch (s) {
        case State::Boot: return "BOOT";
        case State::Spinup: return "SPINUP";
        case State::Run: return "RUN";
        case State::Spindown: return "SPINDOWN";
        case State::WaitPower: return "WAIT_POWER";
        case State::Halted: return "HALTED";
    }
    return "?";
}

State state = State::Boot;
Settings settings;
char pack[64] = {0};

// The serial 'n' command picks a pack by index and reboots. The index
// survives the reboot in watchdog scratch registers, which keep their value
// across resets until power is removed. scratch[0..3] are free for
// applications; the SDK uses 4..7 for its own reboot handling.
constexpr uint32_t PACK_OVERRIDE_MAGIC = 0x5041434B;  // "PACK"

// One shared ring for the one-shot samples (spinup at boot, spindown if it
// has to stream), plus one per looping voice.
SampleRing ringOneShot;
SampleRing ringIdle;
SampleRing ringAccess;

sd::Stream streamSpinup, streamIdle, streamAccess, streamSpindown;

int16_t* spindownRam = nullptr;
uint32_t spindownSamples = 0;
bool spindownArmed = false;  // ready to play instantly (RAM, or primed stream)

bool lastAccess = false;
bool stressOn = false;
uint32_t stressNextMs = 0;
uint32_t stressSpindownStopMs = 0;
uint32_t stressTransitions = 0;
uint32_t stressFailures = 0;

uint32_t lastHeartbeat = 0;
uint32_t lastHeartbeatChangeMs = 0;
bool core1Stalled = false;

uint32_t nextStatsMs = 0;
uint32_t nextPowerTraceMs = 0;
uint32_t lastLoopStartUs = 0;
uint32_t maxLoopGapUs = 0;  // longest gap between loop() passes since power loss

// ---- helpers ---------------------------------------------------------------

void feedWatchdog() {
    if (!core1Stalled) rp2040.wdt_reset();
}

void checkCore1() {
    const uint32_t hb = audio::heartbeat();
    const uint32_t now = millis();
    if (hb != lastHeartbeat) {
        lastHeartbeat = hb;
        lastHeartbeatChangeMs = now;
    } else if (!core1Stalled && now - lastHeartbeatChangeMs > cfg::CORE1_STALL_MS) {
        core1Stalled = true;  // stop feeding: the watchdog resets us
        Serial.println("[fatal] audio core stalled, letting watchdog reset");
    }
}

void samplePath(char* out, size_t len, const char* file) {
    snprintf(out, len, "/samples/%s/%s", pack, file);
}

uint16_t idleLevel() {
    return settings.playIdle ? levelFromFloat(settings.volume * (1.0f - settings.balance)) : 0;
}
uint16_t accessLevel(bool active) {
    return active ? levelFromFloat(settings.volume * settings.balance) : 0;
}

void halt(const char* why) {
    Serial.printf("[fatal] %s\n", why);
    io::ampOff();
    state = State::Halted;
}

void powerTrace(const char* tag, uint32_t intervalMs = 100) {
    const uint32_t now = millis();
    if (static_cast<int32_t>(now - nextPowerTraceMs) < 0) return;
    nextPowerTraceMs = now + intervalMs;
    powerlog::logf("[trace] %s t=%lu supercap=%dmV flt=%d", tag, static_cast<unsigned long>(now),
                   power::supercapMillivolts(), power::fltHigh());
}

// Called by power::waitForRestore() every 100 ms after spindown, until power
// returns or the supercap runs out. The slower trace here is the "how long
// does the board survive" curve; the power log keeps it if we never return.
void waitPowerIdle() {
    feedWatchdog();
    powerTrace("off", 500);
    powerlog::service();
}

uint32_t samplesToMs(uint32_t samples) { return samples * 1000 / cfg::SAMPLE_RATE; }

// Spindown: keep it in RAM if it fits, otherwise keep a primed stream on the
// one-shot ring so it can start instantly. Requires the spinup voice idle.
bool armSpindown() {
    spindownArmed = false;
    if (spindownRam) {
        spindownArmed = true;
        return true;
    }
    char p[128];
    samplePath(p, sizeof(p), "spindown.wav");
    if (!streamSpindown.open &&
        !sd::openStream(streamSpindown, p, VOICE_SPINDOWN, &ringOneShot, false)) {
        return false;
    }
    spindownArmed = sd::prime(streamSpindown);
    return spindownArmed;
}

bool startSpindownVoice() {
    const uint16_t level = levelFromFloat(settings.volume);
    if (spindownRam) return audio::startMemory(VOICE_SPINDOWN, spindownRam, spindownSamples, false, level);
    if (!spindownArmed && !armSpindown()) return false;
    spindownArmed = false;  // the primed ring is consumed by this start
    return audio::startStream(VOICE_SPINDOWN, &ringOneShot, level);
}

bool restartLoop(VoiceId id, sd::Stream& s, uint16_t level) {
    if (!audio::stop(id)) return false;
    if (!sd::prime(s)) return false;
    return audio::startStream(id, s.ring, level);
}

// ---- stats -----------------------------------------------------------------

void printVoice(const char* name, VoiceId id) {
    Voice& v = audio::voice(id);
    const uint32_t minFill = v.minRingFill.load();
    Serial.printf(" | %s: %s under=%lu min_ring=", name, audio::isPlaying(id) ? "on" : "off",
                  static_cast<unsigned long>(v.underrunSamples.load()));
    if (minFill == 0xFFFFFFFF) Serial.print("-");
    else Serial.printf("%lums", static_cast<unsigned long>(samplesToMs(minFill)));
}

void printStats() {
    sd::Stats& st = sd::stats();
    Serial.printf("[stats] t=%lus state=%s heap=%d mix_max=%luus i2s_underflow=%d",
                  static_cast<unsigned long>(millis() / 1000), stateName(state), rp2040.getFreeHeap(),
                  static_cast<unsigned long>(audio::maxMixMicros()), audio::i2sUnderflowed());
    printVoice("idle", VOICE_IDLE);
    printVoice("access", VOICE_ACCESS);
    printVoice("spindown", VOICE_SPINDOWN);
    Serial.printf("\n[stats] sd: chunks=%lu read_err=%lu recoveries=%lu failed_rec=%lu max_read=%luus"
                  " | stress: %s transitions=%lu failures=%lu | supercap=%dmV flt=%d\n",
                  static_cast<unsigned long>(st.chunksRead), static_cast<unsigned long>(st.readErrors),
                  static_cast<unsigned long>(st.recoveries), static_cast<unsigned long>(st.failedRecoveries),
                  static_cast<unsigned long>(st.maxReadMicros), stressOn ? "ON" : "off",
                  static_cast<unsigned long>(stressTransitions), static_cast<unsigned long>(stressFailures),
                  power::supercapMillivolts(), power::fltHigh());
    st.maxReadMicros = 0;
}

// ---- stress test -------------------------------------------------------------
// Hammers exactly what broke the CircuitPython build: starting and stopping
// SD-streamed voices (each start re-reads the card) while others are playing.

void stressStep() {
    const uint32_t now = millis();
    if (stressSpindownStopMs && static_cast<int32_t>(now - stressSpindownStopMs) >= 0) {
        stressSpindownStopMs = 0;
        audio::stop(VOICE_SPINDOWN);
        if (!armSpindown()) stressFailures++;
    }
    if (static_cast<int32_t>(now - stressNextMs) < 0) return;
    stressNextMs = now + random(30, 250);

    bool ok = true;
    switch (random(3)) {
        case 0:
            if (settings.playIdle) ok = restartLoop(VOICE_IDLE, streamIdle, idleLevel());
            break;
        case 1:
            ok = restartLoop(VOICE_ACCESS, streamAccess, accessLevel(lastAccess));
            break;
        default:
            if (!stressSpindownStopMs && settings.playSpindown) {
                ok = startSpindownVoice();
                stressSpindownStopMs = now + random(50, 500);
            }
            break;
    }
    stressTransitions++;
    if (!ok) {
        stressFailures++;
        Serial.println("[stress] transition failed");
    }
}

void stopStress() {
    stressOn = false;
    if (stressSpindownStopMs) {
        stressSpindownStopMs = 0;
        audio::stop(VOICE_SPINDOWN);
        armSpindown();
    }
}

void startStress() {
    stressOn = true;
    stressTransitions = stressFailures = 0;
    for (VoiceId id : {VOICE_IDLE, VOICE_ACCESS, VOICE_SPINDOWN}) {
        audio::voice(id).underrunSamples.store(0);
        audio::voice(id).minRingFill.store(0xFFFFFFFF);
    }
    sd::stats() = sd::Stats{};
}

// ---- serial commands ---------------------------------------------------------

void handleSerial() {
    while (Serial.available()) {
        const char c = Serial.read();
        switch (c) {
            case 'h':
                Serial.println("firmware v" FIRMWARE_VERSION);
                Serial.println("h help | s stats | t stress on/off | p simulate power loss | "
                               "a toggle simulated activity | + / - volume | n next pack | l power log | r reboot | b bootloader");
                break;
            case 's': printStats(); break;
            case 't':
                if (state != State::Run) {
                    Serial.println("[stress] only available in RUN");
                } else if (stressOn) {
                    stopStress();
                    Serial.println("[stress] off");
                } else {
                    startStress();
                    Serial.println("[stress] on (counters reset)");
                }
                break;
            case 'p':
                Serial.println("[power] simulated power loss");
                power::simulateLoss();
                break;
            case 'a':
                io::setSimulation(!io::simulation());
                Serial.printf("[activity] simulation %s\n", io::simulation() ? "on" : "off");
                break;
            case '+':
            case '-':
                settings.volume += (c == '+') ? 0.05f : -0.05f;
                if (settings.volume < 0) settings.volume = 0;
                if (settings.volume > 1) settings.volume = 1;
                Serial.printf("[volume] %d%%\n", static_cast<int>(settings.volume * 100 + 0.5f));
                break;
            case 'n': {
                const int next = sd::packIndex(pack) + 1;
                watchdog_hw->scratch[0] = PACK_OVERRIDE_MAGIC;
                watchdog_hw->scratch[1] = static_cast<uint32_t>(next);
                io::ampOff();
                Serial.println("[pack] switching to the next pack, rebooting");
                Serial.flush();
                rp2040.reboot();
                break;
            }
            case 'l': powerlog::printLast(); break;
            case 'r': rp2040.reboot(); break;
            case 'b': rp2040.rebootToBootloader(); break;
            default: break;
        }
    }
}

// ---- states ------------------------------------------------------------------

const char* resetReasonName() {
    switch (rp2040.getResetReason()) {
        case RP2040::PWRON_RESET: return "power on";
        case RP2040::RUN_PIN_RESET: return "reset button";
        case RP2040::SOFT_RESET: return "software";
        case RP2040::WDT_RESET: return "WATCHDOG";
        case RP2040::DEBUG_RESET: return "debugger";
        case RP2040::GLITCH_RESET: return "POWER GLITCH";
        case RP2040::BROWNOUT_RESET: return "BROWNOUT";
        default: return "unknown";
    }
}

void boot() {
    Serial.println("\n[boot] HDD Synth MK5 C++ firmware v" FIRMWARE_VERSION " (proof of concept)");
    Serial.printf("[boot] reset reason: %s\n", resetReasonName());
    io::begin(settings);  // amp off, LEDs off before anything else

    if (!sd::begin()) {
        halt("no SD card");
        return;
    }

    static char json[2048];
    size_t len = 0;
    if (sd::readTextFile("/settings.json", json, sizeof(json), len) && !parseSettingsJson(json, settings)) {
        Serial.println("[settings] settings.json is not valid JSON, using defaults");
    }
    io::setSimulation(settings.simulationMode);
    printSettings(settings);
    power::begin(settings.powerDetection);
    powerlog::begin();

    bool havePack = false;
    if (watchdog_hw->scratch[0] == PACK_OVERRIDE_MAGIC) {
        int count = 0;
        const int index = static_cast<int>(watchdog_hw->scratch[1]);
        havePack = sd::packByIndex(index, pack, sizeof(pack), count);
        if (havePack) Serial.printf("[boot] pack override %d of %d (serial 'n')\n", index % count + 1, count);
    }
    if (!havePack && !sd::choosePack(settings.samplePack, pack, sizeof(pack))) {
        halt("no sample packs in /samples");
        return;
    }
    Serial.printf("[boot] pack: %s\n", pack);

    char p[128];
    samplePath(p, sizeof(p), "idle.wav");
    const bool idleOk = sd::openStream(streamIdle, p, VOICE_IDLE, &ringIdle, true);
    samplePath(p, sizeof(p), "access.wav");
    const bool accessOk = sd::openStream(streamAccess, p, VOICE_ACCESS, &ringAccess, true);
    if (!accessOk || (settings.playIdle && !idleOk)) {
        halt("pack is missing idle.wav/access.wav or they are the wrong format");
        return;
    }

    samplePath(p, sizeof(p), "spindown.wav");
    if (FORCE_STREAMED_SPINDOWN) Serial.println("[boot] test build: FORCE_STREAMED_SPINDOWN");
    if (!FORCE_STREAMED_SPINDOWN && sd::loadToRam(p, spindownRam, spindownSamples)) {
        Serial.printf("[boot] spindown in RAM (%lu ms)\n", static_cast<unsigned long>(samplesToMs(spindownSamples)));
    } else {
        Serial.println("[boot] spindown will stream from SD");
    }
    Serial.printf("[boot] free heap %d\n", rp2040.getFreeHeap());

    rp2040.wdt_begin(cfg::WATCHDOG_MS);
    lastHeartbeatChangeMs = millis();
    io::ampOn();

    samplePath(p, sizeof(p), "spinup.wav");
    if (settings.playSpinup && sd::openStream(streamSpinup, p, VOICE_SPINUP, &ringOneShot, false) &&
        sd::prime(streamSpinup) &&
        audio::startStream(VOICE_SPINUP, &ringOneShot, levelFromFloat(settings.volume))) {
        Serial.println("[boot] spinup");
    }
    // Fill the loop rings while spinup plays, so RUN starts seamlessly.
    if (settings.playIdle) sd::prime(streamIdle);
    sd::prime(streamAccess);
    state = State::Spinup;
}

void enterRun() {
    sd::closeStream(streamSpinup);
    if (!spindownRam) armSpindown();
    if (settings.playIdle) audio::startStream(VOICE_IDLE, &ringIdle, idleLevel());
    audio::startStream(VOICE_ACCESS, &ringAccess, 0);
    Serial.println("[run] idle/access looping");
    audio::i2sUnderflowed();  // clear the flag left by I2S starting before audio existed
    nextStatsMs = millis() + 5000;
    state = State::Run;
}

void enterSpindown() {
    const bool simulated = power::simulated();
    powerlog::start(simulated ? "simulated (serial p)" : "FLT low", simulated);
    powerlog::logf("[power] loss detected, firmware v" FIRMWARE_VERSION ", pack %s, supercap %d mV", pack,
                   power::supercapMillivolts());
    maxLoopGapUs = 0;
    stopStress();
    audio::stop(VOICE_IDLE);
    audio::stop(VOICE_ACCESS);
    if (!audio::isIdle(VOICE_SPINUP)) audio::stop(VOICE_SPINUP);
    sd::closeStream(streamSpinup);  // frees the one-shot ring for a streamed spindown
    io::rgbOff();
    io::hddLed(false);
    if (settings.playSpindown && startSpindownVoice()) {
        powerlog::logf("[power] spindown");
    }
    state = State::Spindown;
}

void runStep() {
    const bool active = io::activity();
    if (active != lastAccess) {
        lastAccess = active;
        io::hddLed(active);
        if (settings.activityLedEnabled) {
            if (active) io::rgb(settings.activityLedColour[0], settings.activityLedColour[1], settings.activityLedColour[2]);
            else io::rgbOff();
        }
    }
    audio::setLevel(VOICE_IDLE, idleLevel());
    audio::setLevel(VOICE_ACCESS, accessLevel(active));
    if (stressOn) stressStep();
}

} // namespace

void setup() {
    Serial.begin(115200);
#if WAIT_FOR_SERIAL_MS > 0
    while (!Serial && millis() < WAIT_FOR_SERIAL_MS) delay(10);
#endif
    randomSeed(rp2040.hwrand32());
}

void loop() {
    const uint32_t loopStartUs = micros();
    if (lastLoopStartUs && loopStartUs - lastLoopStartUs > maxLoopGapUs) maxLoopGapUs = loopStartUs - lastLoopStartUs;
    lastLoopStartUs = loopStartUs;
    powerlog::service();
    feedWatchdog();
    checkCore1();
    handleSerial();

    switch (state) {
        case State::Boot:
            boot();
            break;

        case State::Spinup:
            sd::pump();
            powerTrace("spinup");
            if (power::lost()) enterSpindown();
            else if (!audio::isPlaying(VOICE_SPINUP)) enterRun();
            break;

        case State::Run:
            sd::pump();
            if (power::lost()) enterSpindown();
            else runStep();
            break;

        case State::Spindown:
            sd::pump();
            powerTrace("spindown");
            if (!audio::isPlaying(VOICE_SPINDOWN)) {
                io::ampOff();
                powerlog::logf("[power] spindown complete, supercap %d mV, longest core 0 stall %lu us",
                               power::supercapMillivolts(), static_cast<unsigned long>(maxLoopGapUs));
                state = State::WaitPower;
            }
            break;

        case State::WaitPower:
            power::waitForRestore(waitPowerIdle);
            powerlog::logf("[power] restarting");
            powerlog::flush();
            Serial.flush();
            rp2040.reboot();
            break;

        case State::Halted:
            // Blink red so a headless unit shows the fault; serial stays alive.
            if ((millis() / 500) % 2) io::rgb(255, 0, 0);
            else io::rgbOff();
            delay(10);
            break;
    }

    if (state == State::Run && static_cast<int32_t>(millis() - nextStatsMs) >= 0) {
        nextStatsMs = millis() + 5000;
        printStats();
    }
}
