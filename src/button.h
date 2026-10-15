// Push-button debouncing and press classification. Pure C++ (no Arduino),
// host-tested: feed it the raw pressed state and the time.
//
// Matches the CircuitPython action_button.py: a long press fires while the
// button is still held (so the user gets feedback without letting go), and a
// short press fires on release, only if no long press fired and it was
// released quickly enough. A press between the two limits does nothing.
#pragma once

#include <stdint.h>

class Button {
public:
    enum class Event { None, Pressed, ShortPress, LongPress };

    Button(uint32_t debounceMs, uint32_t shortMaxMs, uint32_t longMs)
        : debounceMs_(debounceMs), shortMaxMs_(shortMaxMs), longMs_(longMs) {}

    // `pressed` is the raw (bouncy) pin state. At most one event per call.
    Event update(bool pressed, uint32_t nowMs) {
        if (pressed != lastRaw_) {
            lastRaw_ = pressed;
            rawChangedMs_ = nowMs;
        }
        if (pressed != stable_ && nowMs - rawChangedMs_ >= debounceMs_) {
            stable_ = pressed;
            if (stable_) {
                pressedAtMs_ = nowMs;
                longFired_ = false;
                return Event::Pressed;
            }
            if (!longFired_ && nowMs - pressedAtMs_ < shortMaxMs_) return Event::ShortPress;
            return Event::None;
        }
        if (stable_ && !longFired_ && nowMs - pressedAtMs_ >= longMs_) {
            longFired_ = true;
            return Event::LongPress;
        }
        return Event::None;
    }

    bool held() const { return stable_; }

private:
    uint32_t debounceMs_, shortMaxMs_, longMs_;
    bool lastRaw_ = false;
    bool stable_ = false;
    bool longFired_ = false;
    uint32_t rawChangedMs_ = 0;
    uint32_t pressedAtMs_ = 0;
};
