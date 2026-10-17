#include "power.h"

#include <Arduino.h>
#include <atomic>

#include "config.h"
#include "console.h"

namespace power {

namespace {

bool enabled = true;
std::atomic<bool> fltFell{false};
std::atomic<uint32_t> fltFellAtMs{0};
bool lostLatched = false;
bool simulatedLoss = false;

void onFltFalling() {
    // Interrupt context: just timestamp it; confirmation happens in lost().
    fltFellAtMs.store(millis(), std::memory_order_relaxed);
    fltFell.store(true, std::memory_order_release);
}

} // namespace

void begin(bool detectionEnabled) {
    enabled = detectionEnabled;
    pinMode(cfg::PIN_POWER_FLT, INPUT);  // external 10k pull-up (R35)
    analogReadResolution(12);
    analogRead(cfg::PIN_SUPERCAP_ADC);  // the first reading after configuring the pin is low; discard it
    if (enabled) attachInterrupt(digitalPinToInterrupt(cfg::PIN_POWER_FLT), onFltFalling, FALLING);
}

bool fltHigh() { return digitalRead(cfg::PIN_POWER_FLT) == HIGH; }

bool lost() {
    if (lostLatched) return true;
    if (simulatedLoss) return lostLatched = true;
    if (!enabled) return false;

    if (fltHigh()) {
        fltFell.store(false, std::memory_order_relaxed);  // no fault, or a glitch that cleared
        return false;
    }
    // FLT is low. The interrupt normally timestamped the edge already; if FLT
    // was low without an edge (e.g. at boot) start the confirm timer now.
    if (!fltFell.load(std::memory_order_acquire)) onFltFalling();
    if (millis() - fltFellAtMs.load(std::memory_order_relaxed) >= cfg::POWER_LOSS_CONFIRM_MS) {
        lostLatched = true;
    }
    return lostLatched;
}

void simulateLoss() { simulatedLoss = true; }
bool simulated() { return simulatedLoss; }

int supercapMillivolts() { return static_cast<int>(supercapVolts() * 1000.0f + 0.5f); }

float supercapVolts() {
    uint32_t total = 0;
    for (int i = 0; i < 16; ++i) total += analogRead(cfg::PIN_SUPERCAP_ADC);
    const float pin = (total / 16.0f) / 4095.0f * cfg::ADC_REF_V;
    return pin / cfg::SUPERCAP_DIVIDER;
}

void waitForRestore(void (*idle)()) {
    if (!enabled || simulatedLoss) {
        // Nothing real to wait for: hold the low-power wait for 10 s (long
        // enough to read a USB power meter), then restart.
        for (int i = 0; i < 100; ++i) {
            idle();
            delay(100);
        }
        return;
    }
    // FLT alone isn't trustworthy once the eFuse input has decayed (R35 pulls it
    // back high), so also require the supercap to be charging again.
    float baseline = supercapVolts();
    uint32_t windowStart = millis();
    for (;;) {
        idle();
        delay(100);
        const float v = supercapVolts();
        if (!fltHigh()) {
            baseline = v;
            windowStart = millis();
            continue;
        }
        if (v < baseline) baseline = v;
        if (v - baseline >= cfg::RESTORE_RISE_V) {
            Console.printf("[power] restored (supercap %d mV -> %d mV)\n", static_cast<int>(baseline * 1000),
                          static_cast<int>(v * 1000));
            return;
        }
        if (millis() - windowStart >= cfg::RESTORE_WINDOW_MS) {
            baseline = v;
            windowStart = millis();
        }
    }
}

} // namespace power
