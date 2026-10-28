// HDD Synth MK5 firmware: proof of concept.
//
// Core 1: audio engine only (mixer -> I2S), reads RAM, never touches the SD card.
// Core 0: everything else, and the single owner of the SD card.
//
// Serial commands (115200, USB): h help, s stats, t stress test on/off,
// p simulate power loss, a toggle simulated activity, + / - volume,
// n next sample pack, k play the next beep profile, l print the last power-loss log,
// r reboot, b reboot to USB bootloader.
#include <Arduino.h>
#include <hardware/clocks.h>

#include <initializer_list>

#include "audio_engine.h"
#include "beeps.h"
#include "colour.h"
#include "config.h"
#include "console.h"
#include "controls.h"
#include "hdd_link.h"
#include "io.h"
#include "level_follower.h"
#include "oled.h"
#include "pack_info.h"
#include "persist.h"
#include "power.h"
#include "powerlog.h"
#include "saved_levels.h"
#include "sd_streamer.h"
#include "settings.h"
#include "usb_drive.h"
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

enum class State { Boot, Jingle, Spinup, Run, DemoSpindown, StandbySpindown, Standby, Spindown, WaitPower, Halted };
const char* stateName(State s) {
    switch (s) {
        case State::Boot: return "BOOT";
        case State::Jingle: return "JINGLE";
        case State::Spinup: return "SPINUP";
        case State::Run: return "RUN";
        case State::DemoSpindown: return "DEMO_SPINDOWN";
        case State::StandbySpindown: return "STANDBY_SPINDOWN";
        case State::Standby: return "STANDBY";
        case State::Spindown: return "SPINDOWN";
        case State::WaitPower: return "WAIT_POWER";
        case State::Halted: return "HALTED";
    }
    return "?";
}

State state = State::Boot;
Settings settings;
char pack[64] = {0};
PackInfo packInfo;                  // the loaded pack's pack.json (defaults if it has none)
uint8_t settingsLedColour[3] = {0};  // settings.json's activity colour, for packs without their own
beeps::Type haltBeep = beeps::Type::NoSdCard;  // the error the red LED repeats in HALTED
bool haltButtonSeenUp = false;  // HALTED: the action button has been released since (guards factory reset)
uint32_t nextCardCheckMs = 0;   // HALTED with no SD card: when to look for one again
uint32_t cardChecks = 0;        // ...and how many times it has

// One shared ring for the one-shot samples (spinup at boot, spindown if it
// has to stream), plus one per looping voice.
SampleRing ringOneShot;
SampleRing ringIdle;
SampleRing ringAccess;
// sequential.wav's ring: allocated at boot only with
// EXPERIMENTAL_HDDSYNTHPRO_SUPPORT, so without it the RAM is free as before.
SampleRing* ringSequential = nullptr;

sd::Stream streamSpinup, streamIdle, streamAccess, streamSpindown, streamSequential;
bool haveSequential = false;  // the loaded pack has a usable sequential.wav (else access.wav covers it)
AccessChooser accessChooser;  // access.wav or sequential.wav, from the link's activity class
bool ledSequential = false;   // the activity LED shows the sequential colour

int16_t* spindownRam = nullptr;
uint32_t spindownSamples = 0;
bool spindownArmed = false;  // ready to play instantly (RAM, or primed stream)

bool lastAccess = false;
// The access loop plays all the time and activity only changes its level
// (as in the CircuitPython firmware). Once a burst ends and the voice is
// silent, its loop is moved to a random point, so the next burst never
// picks up where the last left off: regular activity (e.g. a burst every
// 10 s) would otherwise land at the same point of a ~10 s loop each time.
uint32_t accessOffMs = 0;
bool accessJumpPending = false;
bool stressOn = false;
uint32_t stressNextMs = 0;
uint32_t stressSpindownStopMs = 0;
uint32_t stressTransitions = 0;
uint32_t stressFailures = 0;

uint32_t lastHeartbeat = 0;
uint32_t lastHeartbeatChangeMs = 0;
bool core1Stalled = false;
bool lowPower = false;  // core 1 deliberately parked after the spindown

uint32_t nextStatsMs = 0;
uint32_t demoEndsMs = 0;  // DEMO_MODE: when the current pack spins down
uint32_t lastActivityMs = 0;  // STANDBY_MINUTES: the last time the drive was active
uint32_t nextPowerTraceMs = 0;
uint32_t lastLoopStartUs = 0;
uint32_t maxLoopGapUs = 0;  // longest gap between loop() passes since power loss

void enterRun();
void startSpinup();
void pickDemoColour();
bool startSpindownVoice();
void changePack(bool spinUp = false);
void enterStandby(bool forced = false);
void factoryReset();
bool startJingle();

// ---- helpers ---------------------------------------------------------------

void feedWatchdog() {
    if (!core1Stalled) rp2040.wdt_reset();
}

// Every planned reboot goes through here. The SD card stays powered through
// it, so leave it idle: one left mid-transfer ignores the next boot's CMD0.
[[noreturn]] void reboot(bool toBootloader = false) {
    sd::finishTransfers();
    Serial.flush();
    if (toBootloader) rp2040.rebootToBootloader();
    else rp2040.reboot();
    for (;;) {}
}

void checkCore1() {
    if (lowPower) return;  // parked on purpose: a still heartbeat isn't a hang
    const uint32_t hb = audio::heartbeat();
    const uint32_t now = millis();
    if (hb != lastHeartbeat) {
        lastHeartbeat = hb;
        lastHeartbeatChangeMs = now;
    } else if (!core1Stalled && now - lastHeartbeatChangeMs > cfg::CORE1_STALL_MS) {
        core1Stalled = true;  // stop feeding: the watchdog resets us
        Console.println("[fatal] audio core stalled, letting watchdog reset");
    }
}

void samplePath(char* out, size_t len, const char* file) {
    snprintf(out, len, "/samples/%s/%s", pack, file);
}

// The drive's own sound: spinup, idle and spindown all play at
// volume x (1 - balance), as in the CircuitPython firmware (they share its
// voice 0), so the handovers between them never change level. Independent of
// PLAY_IDLE. The jingle is the exception: always full volume.
uint16_t baseLevel() { return levelFromFloat(settings.volume * (1.0f - settings.balance) * packInfo.gain); }
uint16_t idleLevel() { return settings.playIdle ? baseLevel() : 0; }
uint16_t accessLevel(bool active) {
    return active ? levelFromFloat(settings.volume * settings.balance * packInfo.gain) : 0;
}

// Beep and wait (amp on), calling feedWatchdog() meanwhile. Leaves the amp on.
void beepAndWait(beeps::Type t) {
    io::ampOn();
    beeps::play(t);
    beeps::waitDone(feedWatchdog);
}

// Fatal: beep the error once, then repeat its pattern on the red LED
// (_halt_on_boot_error() in the CircuitPython firmware).
void halt(const char* why, beeps::Type error) {
    Console.printf("[fatal] %s\n", why);
    for (VoiceId id : {VOICE_SPINUP, VOICE_IDLE, VOICE_ACCESS, VOICE_SEQUENTIAL, VOICE_SPINDOWN}) audio::stop(id);
    beepAndWait(error);
    io::ampOff();
    haltBeep = error;
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
// After the spindown, while waiting on the supercap for power to return.
// The first real cut drew ~130 mW here, mostly core 1 mixing silence at
// 150 MHz; this cuts it to a fraction, so short outages are ridden out and
// the supercap drains far more gently. One-way: the exit is a reboot.
void enterLowPower() {
    for (sd::Stream* s : {&streamSpinup, &streamIdle, &streamAccess, &streamSequential, &streamSpindown}) sd::closeStream(*s);
    io::ledsOffForLowPower();
    lowPower = true;
    const bool parked = audio::shutdownForLowPower();
    // Only change the clock once core 1 has stopped I2S (PIO and DMA run
    // from clk_sys). set_sys_clock_48mhz() runs clk_sys from PLL_USB and
    // turns PLL_SYS off; USB, the ADC, the timer and the watchdog have their
    // own clocks, and SPI to the SD card just runs slower.
    if (parked) set_sys_clock_48mhz();
    powerlog::logf("[power] low-power wait: core 1 %s, clk_sys %lu MHz", parked ? "parked" : "NOT parked",
                   static_cast<unsigned long>(clock_get_hz(clk_sys) / 1000000));
}

void waitPowerIdle() {
    feedWatchdog();
    powerTrace("off", 500);
    powerlog::service();
}

uint32_t packRate = cfg::DEFAULT_SAMPLE_RATE;  // sample rate of the loaded pack (the output runs at it)

uint32_t samplesToMs(uint32_t samples) { return static_cast<uint32_t>(uint64_t(samples) * 1000 / packRate); }

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
    const uint16_t level = baseLevel();
    if (spindownRam) return audio::startMemory(VOICE_SPINDOWN, spindownRam, spindownSamples, false, level);
    if (!spindownArmed && !armSpindown()) return false;
    spindownArmed = false;  // the primed ring is consumed by this start
    return audio::startStream(VOICE_SPINDOWN, &ringOneShot, level);
}

// Fill the looping voices' rings, ready for enterRun().
void primeLoops() {
    if (settings.playIdle) sd::prime(streamIdle);
    sd::prime(streamAccess);
    if (haveSequential) sd::prime(streamSequential);
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
    Console.printf(" | %s: %s under=%lu min_ring=", name, audio::isPlaying(id) ? "on" : "off",
                  static_cast<unsigned long>(v.underrunSamples.load()));
    if (minFill == 0xFFFFFFFF) Console.print("-");
    else Console.printf("%lums", static_cast<unsigned long>(samplesToMs(minFill)));
}

void printStats() {
    sd::Stats& st = sd::stats();
    Console.printf("[stats] t=%lus state=%s heap=%d mix_max=%luus i2s_underflow=%d",
                  static_cast<unsigned long>(millis() / 1000), stateName(state), rp2040.getFreeHeap(),
                  static_cast<unsigned long>(audio::maxMixMicros()), audio::i2sUnderflowed());
    printVoice("idle", VOICE_IDLE);
    printVoice("access", VOICE_ACCESS);
    if (haveSequential) printVoice("sequential", VOICE_SEQUENTIAL);
    printVoice("spindown", VOICE_SPINDOWN);
    Console.printf("\n[stats] sd: chunks=%lu read_err=%lu recoveries=%lu failed_rec=%lu rescued=%lu max_read=%luus"
                  " | stress: %s transitions=%lu failures=%lu | supercap=%dmV flt=%d\n",
                  static_cast<unsigned long>(st.chunksRead), static_cast<unsigned long>(st.readErrors),
                  static_cast<unsigned long>(st.recoveries), static_cast<unsigned long>(st.failedRecoveries),
                  static_cast<unsigned long>(st.rescues), static_cast<unsigned long>(st.maxReadMicros), stressOn ? "ON" : "off",
                  static_cast<unsigned long>(stressTransitions), static_cast<unsigned long>(stressFailures),
                  power::supercapMillivolts(), power::fltHigh());
    st.maxReadMicros = 0;
    hddlink::printStats();
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
        Console.println("[stress] transition failed");
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
                Console.println("firmware v" FIRMWARE_VERSION);
                Console.println("h help | s stats | t stress on/off | p simulate power loss | "
                               "a toggle simulated activity | + / - volume | > / < balance | n next pack | k beep test | l power log | d list SD root | m demo mode | o OLED mode | z standby | j replay jingle | r reboot | b bootloader");
                break;
            case 's': printStats(); break;
            case 't':
                if (state != State::Run) {
                    Console.println("[stress] only available in RUN");
                } else if (stressOn) {
                    stopStress();
                    Console.println("[stress] off");
                } else {
                    startStress();
                    Console.println("[stress] on (counters reset)");
                }
                break;
            case 'p':
                Console.println("[power] simulated power loss");
                power::simulateLoss();
                break;
            case 'a':
                io::setSimulation(!io::simulation());
                Console.printf("[activity] simulation %s\n", io::simulation() ? "on" : "off");
                break;
            case '+':
            case '-':
                controls::adjustVolume(c == '+' ? 1 : -1);
                break;
            case '>':
            case '<':
                controls::adjustBalance(c == '>' ? 1 : -1);
                break;
            case 'n':
                if (state == State::Run) changePack();
                else Console.println("[pack] only in RUN");
                break;
            case 'k': {  // audition the beeps, one profile per press
                static int next = 0;
                static const char* const names[] = {"CHANGE_PACK", "FACTORY_RESET", "VOLUME_MODE", "BALANCE_MODE",
                                                    "NO_SD_CARD", "NO_PACKS_ON_SD_CARD", "CORRUPTED_PACK",
                                                    "PACK_NOT_FOUND"};
                Console.printf("[beep] %s\n", names[next]);
                beeps::play(static_cast<beeps::Type>(next));
                next = (next + 1) % 8;
                break;
            }
            case 'l': powerlog::printLast(); break;
            case 'd': sd::listDir("/"); break;
            case 'z':  // standby now, for testing (as if STANDBY_MINUTES had passed)
                if (state == State::Run) enterStandby(/*forced*/ true);
                else Console.println("[standby] only from RUN");
                break;
            case 'o': {  // cycle the OLED mode, for testing (not saved)
                static const char* const names[] = {"activity", "supercap", "debug"};
                const int next = (static_cast<int>(settings.oledMode) + 1) % 3;
                settings.oledMode = static_cast<OledMode>(next);
                Console.printf("[oled] mode %s\n", names[next]);
                break;
            }
            case 'j':  // replay the jingle: clear its flag and reboot
                for (VoiceId id : {VOICE_SPINUP, VOICE_IDLE, VOICE_ACCESS, VOICE_SEQUENTIAL, VOICE_SPINDOWN}) audio::stop(id);
                io::ampOff();  // audio stopped: safe to write flash
                persist::state().jinglePlayed = false;
                persist::commit();
                Console.println("[boot] jingle flag cleared, rebooting");
                reboot();
                break;
            case 'm':  // demo mode on/off without editing settings.json (not saved)
                settings.demoMode = !settings.demoMode;
                if (settings.demoMode) {
                    settings.simulationMode = true;
                    io::setSimulation(true);
                    pickDemoColour();
                    demoEndsMs = millis() + cfg::DEMO_PACK_MS;
                }
                Console.printf("[demo] %s\n", settings.demoMode ? "on" : "off");
                break;
            case 'r': reboot(); break;
            case 'b': reboot(/*toBootloader*/ true); break;
            default: break;
        }
    }
}

// ---- states ------------------------------------------------------------------

// ---- sample packs ------------------------------------------------------------

// Open idle/access for `pack` and load its spindown into RAM (or leave it to
// stream; enterRun() arms it). Logs and returns false if the pack is unusable.
// All of a pack's WAVs must share one sample rate: the audio output runs at
// that rate while the pack is loaded. Checks the files that exist (a missing
// one is reported where it's needed) and sets packRate.
bool checkPackRate() {
    static const char* const files[] = {"idle.wav", "access.wav", "spinup.wav", "spindown.wav", "sequential.wav"};
    const char* first = nullptr;
    uint32_t rate = 0;
    char p[128];
    for (const char* name : files) {
        if (!ringSequential && !strcmp(name, "sequential.wav")) continue;  // only used with HDDSynthPro support
        samplePath(p, sizeof(p), name);
        WavInfo w;
        if (!sd::wavInfo(p, w)) continue;
        if (!wavIsPlayable(w)) {
            Console.printf("[pack] %s: %s is %lu Hz, %u channel(s), %u bit; needs mono 16-bit at 16000, 22050, "
                           "32000, 44100 or 48000 Hz\n",
                           pack, name, static_cast<unsigned long>(w.sampleRate), w.channels, w.bitsPerSample);
            return false;
        }
        if (!first) {
            first = name;
            rate = w.sampleRate;
        } else if (w.sampleRate != rate) {
            Console.printf("[pack] %s: %s is %lu Hz but %s is %lu Hz; all of a pack's WAVs must share one rate\n",
                           pack, name, static_cast<unsigned long>(w.sampleRate), first, static_cast<unsigned long>(rate));
            return false;
        }
    }
    if (rate) packRate = rate;
    return true;
}

// pack.json (optional): name, author, gain, activity colour, starting balance.
void loadPackInfo() {
    char p[128];
    samplePath(p, sizeof(p), "pack.json");
    static char json[1024];
    size_t len = 0;
    packInfo = PackInfo{};
    if (sd::readTextFile(p, json, sizeof(json), len) && !parsePackJson(json, packInfo)) {
        Console.printf("[pack] %s: pack.json is not valid JSON, ignoring it\n", pack);
    }
    if (packInfo.name[0] || packInfo.author[0] || packInfo.gain != 1.0f) {
        Console.printf("[pack] %s%s%s, gain %d%%\n", packInfo.name[0] ? packInfo.name : pack,
                       packInfo.author[0] ? " by " : "", packInfo.author,
                       static_cast<int>(lroundf(packInfo.gain * 100)));
    }
    oled::setPack(packInfo.name[0] ? packInfo.name : pack);
    // The pack's own activity colour, else settings.json's (demo mode then
    // picks its own random one).
    memcpy(settings.activityLedColour, packInfo.hasLedColour ? packInfo.ledColour : settingsLedColour, 3);
    // Starting balance: settings.json BALANCE_DEFAULT pins it; at boot a
    // saved balance (SAVE_VOLUME_BALANCE) also wins; otherwise the pack's.
    if (packInfo.hasBalance && !settings.balanceFromConfig &&
        !(state == State::Boot && savedlevels::balanceRestored())) {
        settings.balance = packInfo.balance;
        Console.printf("[pack] balance %d%% (pack.json)\n", static_cast<int>(lroundf(packInfo.balance * 100)));
    }
}

bool loadPack() {
    Console.printf("[pack] %s\n", pack);
    loadPackInfo();
    if (!checkPackRate()) return false;
    // Nothing is playing at this point (boot, pack change, demo hand-over).
    audio::setSampleRate(packRate);
    Console.printf("[pack] %lu Hz\n", static_cast<unsigned long>(packRate));
    char p[128];
    samplePath(p, sizeof(p), "idle.wav");
    const bool idleOk = sd::openStream(streamIdle, p, VOICE_IDLE, &ringIdle, true);
    samplePath(p, sizeof(p), "access.wav");
    const bool accessOk = sd::openStream(streamAccess, p, VOICE_ACCESS, &ringAccess, true);
    if (!accessOk || (settings.playIdle && !idleOk)) {
        Console.printf("[pack] %s is missing idle.wav/access.wav or they are the wrong format\n", pack);
        return false;
    }
    // sequential.wav is optional: without it, sequential transfers play access.wav.
    samplePath(p, sizeof(p), "sequential.wav");
    if (ringSequential && sd::fileExists(p)) {
        haveSequential = sd::openStream(streamSequential, p, VOICE_SEQUENTIAL, ringSequential, true);
        Console.printf("[pack] sequential.wav %s\n", haveSequential ? "found" : "unusable, using access.wav");
    }
    samplePath(p, sizeof(p), "spindown.wav");
    if (!FORCE_STREAMED_SPINDOWN && sd::loadToRam(p, spindownRam, spindownSamples)) {
        Console.printf("[pack] spindown in RAM (%lu ms)\n", static_cast<unsigned long>(samplesToMs(spindownSamples)));
    } else {
        Console.println("[pack] spindown will stream from SD");
    }
    return true;
}

// Close the pack's streams and free its spindown. The voices must be stopped.
void unloadPack() {
    sd::closeStream(streamIdle);
    sd::closeStream(streamAccess);
    sd::closeStream(streamSequential);
    haveSequential = false;
    sd::closeStream(streamSpindown);
    free(spindownRam);
    spindownRam = nullptr;
    spindownSamples = 0;
    spindownArmed = false;
}

void stopPackVoices() {
    stopStress();
    for (VoiceId id : {VOICE_IDLE, VOICE_ACCESS, VOICE_SEQUENTIAL, VOICE_SPINDOWN}) audio::stop(id);
    io::rgbOff();
    io::hddLed(false);
    lastAccess = false;
}

// Unload the current pack and load the next usable one into `pack`, skipping
// broken ones. Halts (and returns false) if none is usable. Voices stopped.
bool loadNextPack() {
    unloadPack();
    const int current = sd::packIndex(pack);  // -1 if it has gone: start from the first
    int count = 0;
    for (int step = 1;; ++step) {
        if (!sd::packByIndex(current + step, pack, sizeof(pack), count) || step > count) {
            halt("no usable sample pack", beeps::Type::CorruptedPack);
            return false;
        }
        if (loadPack()) return true;
        unloadPack();  // broken pack: try the next one
    }
}

// ---- demo mode -----------------------------------------------------------------
// DEMO_MODE: each pack spins up, plays idle/access with simulated activity
// for DEMO_PACK_MS, then plays its spindown, and the next pack takes over
// with a new random activity LED colour. The packs are not saved to flash:
// a write per minute would wear the sector out in months.

void pickDemoColour() {
    uint8_t* c = settings.activityLedColour;
    hueToRgb(static_cast<uint16_t>(random(360)), c[0], c[1], c[2]);
    Console.printf("[demo] %s, LED colour %u,%u,%u\n", pack, c[0], c[1], c[2]);
}

// ---- standby -------------------------------------------------------------------
// STANDBY_MINUTES: like a real drive's power management, spin down after a
// spell without HDD activity, and spin up again on the next access.

bool standbyDue() {
    if (!settings.standbyMinutes || settings.demoMode) return false;
    return millis() - lastActivityMs >= settings.standbyMinutes * 60000u;
}

void enterStandby(bool forced) {
    if (forced) Console.println("[standby] spinning down (serial z)");
    else Console.printf("[standby] no activity for %lu min, spinning down\n",
                        static_cast<unsigned long>(settings.standbyMinutes));
    stopPackVoices();
    if (settings.playSpindown) startSpindownVoice();
    state = State::StandbySpindown;
}

void wakeFromStandby() {
    Console.println("[standby] activity, spinning up");
    io::ampOn();
    startSpinup();  // then RUN, as at boot
}

void enterDemoSpindown() {
    Console.println("[demo] spinning down");
    stopPackVoices();
    if (settings.playSpindown) startSpindownVoice();
    state = State::DemoSpindown;
}

void nextDemoPack() {
    if (!loadNextPack()) return;
    pickDemoColour();
    startSpinup();  // then RUN, which restarts the demo timer
}

// Short press of the action button (or serial 'n'): switch to the next pack
// without rebooting, and remember it (sample_changer.next_pack() plus the
// reload callback in the CircuitPython firmware). Spinup is not replayed.
void changePack(bool spinUp) {
    stopPackVoices();
    beepAndWait(beeps::Type::ChangePack);
    io::ampOff();
    if (!loadNextPack()) return;
    PersistedState& saved = persist::state();
    strncpy(saved.desiredPack, pack, sizeof(saved.desiredPack) - 1);
    // The audio is stopped and the amp muted, so pausing core 1 for the flash
    // write is harmless. Don't let the stall detector mistake it for a hang.
    const uint32_t ms = persist::commit();
    if (ms) Console.printf("[persist] saved pack in %lu ms\n", static_cast<unsigned long>(ms));
    lastHeartbeatChangeMs = millis();
    io::ampOn();
    if (spinUp) {
        startSpinup();  // e.g. out of standby: spin up like a real drive
        return;
    }
    primeLoops();
    enterRun();
}

// Long press (3 s) of the action button: forget the chosen pack and the
// jingle flag, then reboot into the defaults (action_button.py).
void factoryReset() {
    Console.println("[pack] factory reset");
    stopPackVoices();
    beepAndWait(beeps::Type::FactoryReset);
    io::ampOff();
    savedlevels::clear();
    persist::state() = PersistedState{};
    persist::commit();
    reboot();
}

// RANDOM_SAMPLE_PACK: pick any pack except the one used last time (when
// there is a choice), into `pack`, and remember it (random_pack()).
// Called at boot before any audio, so the flash write is harmless.
bool chooseRandomPack() {
    int count = 0;
    if (!sd::packByIndex(0, pack, sizeof(pack), count)) return false;
    const int previous = sd::packIndex(persist::state().desiredPack);
    int index = static_cast<int>(random(count));
    if (count > 1 && previous >= 0) {
        index = static_cast<int>(random(count - 1));  // uniform over the others
        if (index >= previous) ++index;
    }
    sd::packByIndex(index, pack, sizeof(pack), count);
    Console.printf("[boot] random pack %d of %d: %s\n", index + 1, count, pack);
    PersistedState& saved = persist::state();
    strncpy(saved.desiredPack, pack, sizeof(saved.desiredPack) - 1);
    persist::commit();
    return true;
}

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
    Console.println("\n[boot] HDD Synth MK5 C++ firmware v" FIRMWARE_VERSION " (proof of concept)");
    Console.printf("[boot] reset reason: %s\n", resetReasonName());
#if WAIT_FOR_SERIAL_MS > 0
    Console.printf("[boot] debug build (USB upload): waited up to %d ms for the serial console\n", WAIT_FOR_SERIAL_MS);
#endif
    io::begin(settings);  // amp off, LEDs off before anything else
    // Buttons next, before anything can fail and halt: an unconfigured pin
    // sits on the RP2350's default pull-down, reads as "pressed", and the
    // halted state's 3 s long press would then factory-reset in a loop.
    controls::begin(settings);
    beeps::begin(settings);

    if (!sd::begin()) {
        halt("no SD card", beeps::Type::NoSdCard);
        return;
    }

    static char json[2048];
    size_t len = 0;
    if (sd::readTextFile("/settings.json", json, sizeof(json), len) && !parseSettingsJson(json, settings)) {
        Console.println("[settings] settings.json is not valid JSON, using defaults");
    }
    if (settings.demoMode) settings.simulationMode = true;  // the demo fakes HDD activity
    io::setSimulation(settings.simulationMode);
    printSettings(settings);
    memcpy(settingsLedColour, settings.activityLedColour, 3);

    // Action button held at power-on: share the SD card over USB instead
    // (checked before anything writes to the card).
    if (usbdrive::requested()) {
        oled::begin(settings);
        oled::showMessage("USB drive", "SD card shared over", "USB. Eject to finish.");
        usbdrive::run();  // reboots when done
    }
    if (settings.hddSynthPro) {
        // The link uses the OLED's pins on J6, so the two can't share them.
        if (settings.oledEnabled) {
            Console.println("[link] EXPERIMENTAL_HDDSYNTHPRO_SUPPORT uses J6: OLED off");
            settings.oledEnabled = false;
        }
        hddlink::begin();
        ringSequential = new SampleRing;  // before the pack loads, so the spindown's RAM check counts it
    }
    power::begin(settings.powerDetection);
    powerlog::begin();
    persist::begin();
    savedlevels::begin(settings);  // before anything uses the volume
    oled::begin(settings);         // before the pack loads, so its 1 KB counts against the spindown's RAM check

    // Which pack: RANDOM_SAMPLE_PACK, else SAMPLE_PACK in settings.json (as in
    // the CircuitPython firmware), then the one last chosen with the action
    // button, then the first pack on the card.
    bool havePack = settings.randomSamplePack && chooseRandomPack();
    if (!havePack) {
        const char* preferred = persist::state().desiredPack;
        if (settings.samplePack[0] && !settings.randomSamplePack) {
            if (sd::packExists(settings.samplePack)) preferred = settings.samplePack;
            else {
                Console.printf("[boot] SAMPLE_PACK '%s' not found\n", settings.samplePack);
                beepAndWait(beeps::Type::PackNotFound);  // a warning: carry on with another pack
                io::ampOff();
            }
        }
        havePack = sd::choosePack(preferred, pack, sizeof(pack));
    }
    if (!havePack) {
        halt("no sample packs in /samples", beeps::Type::NoPacksOnSdCard);
        return;
    }
    if (FORCE_STREAMED_SPINDOWN) Console.println("[boot] test build: FORCE_STREAMED_SPINDOWN");
    if (!loadPack()) {
        halt("pack is missing idle.wav/access.wav or they are the wrong format (hold the action button 3 s to reset)",
             beeps::Type::CorruptedPack);
        return;
    }
    Console.printf("[boot] free heap %d\n", rp2040.getFreeHeap());

    rp2040.wdt_begin(cfg::WATCHDOG_MS);
    lastHeartbeatChangeMs = millis();
    io::ampOn();

    if (settings.demoMode) {
        Console.printf("[demo] on: %lu s per pack, cycling through every pack\n",
                      static_cast<unsigned long>(cfg::DEMO_PACK_MS / 1000));
        pickDemoColour();
    }
    if (startJingle()) state = State::Jingle;  // spinup follows it
    else startSpinup();
}

// While the jingle plays, the RGB LED cycles through the rainbow and pulses
// with the loudness of the audio (core 1 reports each block's peak).
LevelFollower jingleLevel(/*attackMs*/ 15, /*releaseMs*/ 180, /*gainDecayMs*/ 3000, /*floor*/ 0.05f);
uint32_t jingleStartMs = 0;
uint32_t jingleNextLightMs = 0;

void jingleLights() {
    if (beeps::active()) return;  // an encoder beep owns the LED for a moment
    const uint32_t now = millis();
    if (static_cast<int32_t>(now - jingleNextLightMs) < 0) return;
    jingleNextLightMs = now + cfg::JINGLE_LIGHT_STEP_MS;
    const float brightness = jingleLevel.update(audio::outputPeak(), cfg::JINGLE_LIGHT_STEP_MS);
    uint8_t r, g, b;
    hueToRgb(static_cast<uint16_t>((now - jingleStartMs) % cfg::JINGLE_RAINBOW_MS * 360 / cfg::JINGLE_RAINBOW_MS), r, g, b);
    io::rgb(static_cast<uint8_t>(r * brightness), static_cast<uint8_t>(g * brightness),
            static_cast<uint8_t>(b * brightness));
}

// The jingle plays once, on the first boot (or after a factory reset), at
// full volume before the spinup: _maybe_play_jingle() in the CircuitPython
// firmware. It streams through the spinup voice and ring.
bool startJingle() {
    if (persist::state().jinglePlayed && !settings.alwaysPlayJingle) return false;
    if (!sd::openStream(streamSpinup, "/jingle.wav", VOICE_SPINUP, &ringOneShot, false)) {
        Console.println("[boot] no /jingle.wav, skipping the jingle");
        return false;
    }
    // The jingle has its own sample rate (it isn't part of a pack).
    audio::setSampleRate(streamSpinup.info.sampleRate);
    if (!sd::prime(streamSpinup) || !audio::startStream(VOICE_SPINUP, &ringOneShot, LEVEL_UNITY)) {
        audio::setSampleRate(packRate);
        return false;
    }
    Console.printf("[boot] jingle (%lu Hz)\n", static_cast<unsigned long>(streamSpinup.info.sampleRate));
    jingleLevel.reset();
    jingleStartMs = jingleNextLightMs = millis();
    return true;
}

void finishJingle() {
    io::rgbOff();
    sd::closeStream(streamSpinup);
    audio::setSampleRate(packRate);  // back to the pack's rate for the spinup
    if (!persist::state().jinglePlayed) {
        persist::state().jinglePlayed = true;
        // Nothing is playing between the jingle and the spinup, so this is a
        // safe moment for the flash write (it pauses core 1).
        io::ampOff();
        persist::commit();
        lastHeartbeatChangeMs = millis();
        io::ampOn();
    }
    startSpinup();
}

void startSpinup() {
    char p[128];
    samplePath(p, sizeof(p), "spinup.wav");
    if (settings.playSpinup && sd::openStream(streamSpinup, p, VOICE_SPINUP, &ringOneShot, false) &&
        sd::prime(streamSpinup) &&
        audio::startStream(VOICE_SPINUP, &ringOneShot, baseLevel())) {
        Console.println("[boot] spinup");
    }
    // Fill the loop rings while spinup plays, so RUN starts seamlessly.
    primeLoops();
    state = State::Spinup;
}

void enterRun() {
    sd::closeStream(streamSpinup);
    if (!spindownRam) armSpindown();
    if (settings.playIdle) audio::startStream(VOICE_IDLE, &ringIdle, idleLevel());
    audio::startStream(VOICE_ACCESS, &ringAccess, 0);
    if (haveSequential) audio::startStream(VOICE_SEQUENTIAL, ringSequential, 0);
    Console.println("[run] idle/access looping");
    audio::i2sUnderflowed();  // clear the flag left by I2S starting before audio existed
    nextStatsMs = millis() + 5000;
    demoEndsMs = millis() + cfg::DEMO_PACK_MS;
    lastActivityMs = millis();
    state = State::Run;
}

void enterSpindown() {
    oled::off();  // lit, the panel would take a real share of the supercap's energy
    const bool simulated = power::simulated();
    powerlog::start(simulated ? "simulated (serial p)" : "FLT low", simulated);
    powerlog::logf("[power] loss detected, firmware v" FIRMWARE_VERSION ", pack %s, supercap %d mV", pack,
                   power::supercapMillivolts());
    maxLoopGapUs = 0;
    stopStress();
    audio::stop(VOICE_IDLE);
    audio::stop(VOICE_ACCESS);
    audio::stop(VOICE_SEQUENTIAL);
    if (!audio::isIdle(VOICE_SPINUP)) audio::stop(VOICE_SPINUP);
    sd::closeStream(streamSpinup);  // frees the one-shot ring for a streamed spindown
    io::rgbOff();
    io::hddLed(false);
    if ((state == State::DemoSpindown || state == State::StandbySpindown) && audio::isPlaying(VOICE_SPINDOWN)) {
        powerlog::logf("[power] spindown already playing, letting it finish");
    } else if (state == State::Standby) {
        powerlog::logf("[power] in standby: already spun down");
    } else if (settings.playSpindown && startSpindownVoice()) {
        powerlog::logf("[power] spindown");
    }
    state = State::Spindown;
}

// Move a (silent) access or sequential loop to a random point. Refilling its
// ring takes ~20 ms of SD reads, all during the quiet after a burst.
void jumpLoop(VoiceId id, sd::Stream& s) {
    if (!audio::stop(id)) return;  // silent: its level is already 0
    const uint32_t frames = s.info.dataBytes / 2;
    const uint32_t frame = frames ? static_cast<uint32_t>(random(frames)) : 0;
    if (!sd::prime(s, frame * 2) || !audio::startStream(id, s.ring, 0)) {
        Console.printf("[access] couldn't move the %s loop\n", id == VOICE_SEQUENTIAL ? "sequential" : "access");
    }
}

void jumpAccess() {
    jumpLoop(VOICE_ACCESS, streamAccess);
    if (haveSequential) jumpLoop(VOICE_SEQUENTIAL, streamSequential);
}

void runStep() {
    const bool active = io::activity();
    const bool beepEnded = beeps::service();  // a beep owns the RGB LED while it plays
    if (active) lastActivityMs = millis();
    if (lastAccess && !active) {
        accessOffMs = millis();
        accessJumpPending = true;
    }
    if (active) accessJumpPending = false;
    if (!active && accessJumpPending && millis() - accessOffMs >= cfg::ACCESS_JUMP_QUIET_MS) {
        accessJumpPending = false;
        jumpAccess();
    }
    // With HDDSynthPro support: sequential.wav during sequential transfers
    // (access.wav if the pack has none), and the LED in its own colour. The
    // 8 ms level fade covers a switch between the two mid-burst.
    const AccessSample sample = accessChooser.choose(io::activityClass(), haveSequential);
    const bool seqLed = settings.hddSynthPro && accessChooser.sequential();
    if (active != lastAccess || beepEnded || (active && seqLed != ledSequential)) {
        lastAccess = active;
        ledSequential = seqLed;
        io::hddLed(active);
        if (settings.activityLedEnabled && !beeps::active()) {
            const uint8_t* c = seqLed ? settings.sequentialLedColour : settings.activityLedColour;
            if (active) io::rgb(c[0], c[1], c[2]);
            else io::rgbOff();
        }
    }
    audio::setLevel(VOICE_IDLE, idleLevel());
    audio::setLevel(VOICE_ACCESS, accessLevel(active && sample == AccessSample::Access));
    if (haveSequential) audio::setLevel(VOICE_SEQUENTIAL, accessLevel(active && sample == AccessSample::Sequential));
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
    hddlink::service();

    switch (state) {
        case State::Boot:
            boot();
            break;

        case State::Jingle:
            sd::pump();
            oled::service();
            savedlevels::service();
            controls::poll();  // volume changes apply from the spinup on; the jingle is pinned at full
            beeps::service();
            jingleLights();
            if (power::lost()) enterSpindown();
            else if (!audio::isPlaying(VOICE_SPINUP)) finishJingle();
            break;

        case State::Spinup:
            sd::pump();
            oled::service();
            savedlevels::service();
            controls::poll();  // pack changes wait for RUN; volume applies straight away
            audio::setLevel(VOICE_SPINUP, baseLevel());
            beeps::service();
            powerTrace("spinup");
            if (power::lost()) enterSpindown();
            else if (!audio::isPlaying(VOICE_SPINUP)) enterRun();
            break;

        case State::Run: {
            sd::pump();
            oled::service();
            savedlevels::service();
            const controls::Action action = controls::poll();
            if (power::lost()) enterSpindown();
            else if (action == controls::Action::NextPack) settings.demoMode ? enterDemoSpindown() : changePack();
            else if (action == controls::Action::FactoryReset) factoryReset();
            else if (settings.demoMode && static_cast<int32_t>(millis() - demoEndsMs) >= 0) enterDemoSpindown();
            else if (standbyDue()) enterStandby();
            else runStep();
            break;
        }

        case State::DemoSpindown: {
            sd::pump();
            oled::service();
            savedlevels::service();
            const controls::Action action = controls::poll();
            beeps::service();
            audio::setLevel(VOICE_SPINDOWN, baseLevel());
            if (power::lost()) enterSpindown();
            else if (action == controls::Action::FactoryReset) factoryReset();
            else if (!audio::isPlaying(VOICE_SPINDOWN)) nextDemoPack();
            break;
        }

        case State::StandbySpindown: {
            sd::pump();
            oled::service();
            savedlevels::service();
            const controls::Action action = controls::poll();
            beeps::service();
            audio::setLevel(VOICE_SPINDOWN, baseLevel());
            if (power::lost()) enterSpindown();
            else if (action == controls::Action::FactoryReset) factoryReset();
            else if (!audio::isPlaying(VOICE_SPINDOWN)) {
                io::ampOff();
                Console.println("[standby] spun down, waiting for activity");
                state = State::Standby;
            }
            break;
        }

        case State::Standby: {
            oled::service();
            savedlevels::service();
            const controls::Action action = controls::poll();
            beeps::service();
            if (power::lost()) enterSpindown();
            else if (action == controls::Action::NextPack) changePack(/*spinUp*/ true);
            else if (action == controls::Action::FactoryReset) factoryReset();
            else if (io::activity()) wakeFromStandby();
            break;
        }

        case State::Spindown:
            sd::pump();
            powerTrace("spindown");
            if (!audio::isPlaying(VOICE_SPINDOWN)) {
                io::ampOff();
                powerlog::logf("[power] spindown complete, supercap %d mV, longest core 0 stall %lu us",
                               power::supercapMillivolts(), static_cast<unsigned long>(maxLoopGapUs));
                enterLowPower();
                state = State::WaitPower;
            }
            break;

        case State::WaitPower:
            power::waitForRestore(waitPowerIdle);
            powerlog::logf("[power] restarting");
            powerlog::flush();
            reboot();
            break;

        case State::Halted:
            oled::service();  // e.g. the debug screen shows the fatal error
            // A factory reset is still possible (e.g. when the saved pack is
            // broken), but only once the button has been seen released, so a
            // stuck or floating input can't trigger one.
            if (digitalRead(cfg::PIN_ACTION_BUTTON) == HIGH) haltButtonSeenUp = true;
            if (controls::poll() == controls::Action::FactoryReset && haltButtonSeenUp) factoryReset();
            // No SD card: keep looking, and start over once one answers. The
            // quick probe every 2 s; every 10 s SdFat's full init too, which
            // blocks up to 2 s (only the LED pattern pauses).
            if (haltBeep == beeps::Type::NoSdCard && static_cast<int32_t>(millis() - nextCardCheckMs) >= 0) {
                nextCardCheckMs = millis() + 2000;
                if (sd::present() || (++cardChecks % 5 == 0 && sd::mountsNow())) {
                    Console.println("[sd] card found, restarting");
                    reboot();
                }
            }
            // Repeat the error's beep pattern in red so a headless unit shows
            // which fault it is; serial stays alive.
            if (beeps::errorLedOn(haltBeep, millis())) io::rgb(255, 0, 0);
            else io::rgbOff();
            delay(10);
            break;
    }

    if (state == State::Run && static_cast<int32_t>(millis() - nextStatsMs) >= 0) {
        nextStatsMs = millis() + 5000;
        printStats();
    }
}
