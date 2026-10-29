#include "io.h"

#include <Arduino.h>
#include <hardware/pwm.h>

#include <initializer_list>

#include "config.h"
#include "hdd_link.h"
#include "usb_host.h"

namespace io {

namespace {

const Settings* settings = nullptr;
bool simulate = false;
uint32_t lastActiveMs = 0;
bool simState = false;
bool simSequential = false;  // the current simulated burst is a sequential spell
uint32_t simNextSwitchMs = 0;

void writeChannel(uint8_t pin, uint8_t level) {
    // Common anode: LOW = on, so invert the duty.
    analogWrite(pin, 255 - level);
}

} // namespace

void begin(const Settings& s) {
    settings = &s;
    simulate = s.simulationMode;

    pinMode(cfg::PIN_AMP_SDMODE, OUTPUT);
    digitalWrite(cfg::PIN_AMP_SDMODE, LOW);
    pinMode(cfg::PIN_HDD_LED, OUTPUT);
    digitalWrite(cfg::PIN_HDD_LED, LOW);
    pinMode(cfg::PIN_ACTIVITY, INPUT_PULLUP);

    analogWriteFreq(1000);
    analogWriteRange(255);
    rgbOff();
}

void ampOn() { digitalWrite(cfg::PIN_AMP_SDMODE, HIGH); }
void ampOff() { digitalWrite(cfg::PIN_AMP_SDMODE, LOW); }

void rgb(uint8_t r, uint8_t g, uint8_t b) {
    const float k = settings ? settings->rgbBrightness : 0.5f;
    writeChannel(cfg::PIN_RGB_RED, static_cast<uint8_t>(r * k));
    writeChannel(cfg::PIN_RGB_GREEN, static_cast<uint8_t>(g * k));
    writeChannel(cfg::PIN_RGB_BLUE, static_cast<uint8_t>(b * k));
}

void rgbOff() {
    writeChannel(cfg::PIN_RGB_RED, 0);
    writeChannel(cfg::PIN_RGB_GREEN, 0);
    writeChannel(cfg::PIN_RGB_BLUE, 0);
}

void hddLed(bool on) { digitalWrite(cfg::PIN_HDD_LED, on ? HIGH : LOW); }

void ledsOffForLowPower() {
    // rgbOff() leaves PWM running at 100% duty (the LED is common anode, so
    // HIGH = off). As plain outputs held HIGH, the PWM slices can stop.
    for (uint8_t pin : {cfg::PIN_RGB_RED, cfg::PIN_RGB_GREEN, cfg::PIN_RGB_BLUE}) {
        pinMode(pin, OUTPUT);
        digitalWrite(pin, HIGH);
        pwm_set_enabled(pwm_gpio_to_slice_num(pin), false);  // slices 2 and 3: only the RGB LED uses them
    }
    hddLed(false);
}

void setSimulation(bool on) { simulate = on; }
bool simulation() { return simulate; }

bool activity() {
    const uint32_t now = millis();
    if (simulate) {
        if (static_cast<int32_t>(now - simNextSwitchMs) >= 0) {
            simState = random(2);
            // With HDDSynthPro support, one burst in three is a longer
            // sequential transfer.
            simSequential = simState && settings && settings->hddSynthPro && random(3) == 0;
            simNextSwitchMs = now + (simSequential ? random(2000, 6000) : random(0, 2000));
        }
        return simState;
    }
    // The local source: the HDDSynthPro link while present, else the
    // activity input. The USB companion app's activity adds to it.
    const bool local = hddlink::present() ? hddlink::state().busy : digitalRead(cfg::PIN_ACTIVITY) == LOW;
    const bool raw = local || (usbhost::present() && usbhost::state().busy);
    if (raw) {
        lastActiveMs = now;
        return true;
    }
    const uint32_t hold = settings ? settings->accessHoldMs : 100;
    return lastActiveMs != 0 && (now - lastActiveMs) < hold;
}

ActivityClass activityClass() {
    if (simulate) return simSequential ? ActivityClass::Sequential : ActivityClass::Random;
    if (hddlink::present()) return hddlink::state().cls;
    if (usbhost::present()) return usbhost::state().cls;
    return ActivityClass::Random;
}

} // namespace io
