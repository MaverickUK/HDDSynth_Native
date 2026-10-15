#include "controls.h"

#include <Arduino.h>
#include <atomic>

#include "button.h"
#include "config.h"
#include "quadrature.h"

namespace controls {

namespace {

Settings* settings = nullptr;
Mode currentMode = Mode::Volume;

// Encoder: decoded in the pin interrupts (so no transition is missed while
// loop() is busy with the SD card), consumed by poll().
QuadratureDecoder decoder;
std::atomic<int32_t> pendingDetents{0};

uint8_t readAB() {
    return static_cast<uint8_t>((digitalRead(cfg::PIN_ENC_A) << 1) | digitalRead(cfg::PIN_ENC_B));
}

void onEncoderEdge() {
    // Both pins share this handler; the decoder only looks at the new state.
    const int detent = decoder.update(readAB());
    if (detent) pendingDetents.fetch_add(detent, std::memory_order_relaxed);
}

// Buttons are active low (pressed = pin pulled to ground). The encoder
// button only uses Pressed, so its press-length limits don't matter.
Button encoderButton(cfg::BUTTON_DEBOUNCE_MS, UINT32_MAX, UINT32_MAX);
Button actionButton(cfg::BUTTON_DEBOUNCE_MS, cfg::ACTION_SHORT_PRESS_MAX_MS, cfg::ACTION_LONG_PRESS_MS);

// Move a 0..1 value by whole percent steps. Working in integer percent keeps
// repeated steps exact (0.05 isn't exact in binary floating point).
bool adjustPercent(float& value, int steps, int stepPercent) {
    long pct = lroundf(value * 100.0f) + static_cast<long>(steps) * stepPercent;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    const float next = pct / 100.0f;
    if (next == value) return false;
    value = next;
    return true;
}

} // namespace

void begin(Settings& s) {
    settings = &s;
    pinMode(cfg::PIN_ENC_A, INPUT_PULLUP);
    pinMode(cfg::PIN_ENC_B, INPUT_PULLUP);
    pinMode(cfg::PIN_ENC_BUTTON, INPUT_PULLUP);
    pinMode(cfg::PIN_ACTION_BUTTON, INPUT_PULLUP);
    decoder = QuadratureDecoder(readAB());
    // Attached from core 0, so the interrupts run on core 0 and never
    // disturb the audio core.
    attachInterrupt(digitalPinToInterrupt(cfg::PIN_ENC_A), onEncoderEdge, CHANGE);
    attachInterrupt(digitalPinToInterrupt(cfg::PIN_ENC_B), onEncoderEdge, CHANGE);
}

bool adjustVolume(int steps) {
    if (!settings || !adjustPercent(settings->volume, steps, cfg::VOLUME_STEP_PERCENT)) return false;
    Serial.printf("[volume] %d%%\n", static_cast<int>(lroundf(settings->volume * 100)));
    return true;
}

bool adjustBalance(int steps) {
    if (!settings || !adjustPercent(settings->balance, steps, cfg::BALANCE_STEP_PERCENT)) return false;
    Serial.printf("[balance] %d%% access\n", static_cast<int>(lroundf(settings->balance * 100)));
    return true;
}

Action poll() {
    const uint32_t now = millis();
    if (encoderButton.update(digitalRead(cfg::PIN_ENC_BUTTON) == LOW, now) == Button::Event::Pressed) {
        currentMode = currentMode == Mode::Volume ? Mode::Balance : Mode::Volume;
        Serial.printf("[encoder] mode: %s\n", currentMode == Mode::Volume ? "volume" : "balance");
        // TODO(beeps): 2 short high beeps for volume, 3 for balance (beep_profiles.py).
    }
    int32_t detents = pendingDetents.exchange(0, std::memory_order_relaxed);
    if (detents != 0) {
        if (cfg::ENCODER_REVERSED) detents = -detents;
        if (currentMode == Mode::Volume) adjustVolume(detents);
        else adjustBalance(detents);
    }
    switch (actionButton.update(digitalRead(cfg::PIN_ACTION_BUTTON) == LOW, now)) {
        case Button::Event::ShortPress: return Action::NextPack;
        case Button::Event::LongPress: return Action::FactoryReset;
        default: return Action::None;
    }
}

Mode mode() { return currentMode; }

} // namespace controls
