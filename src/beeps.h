// Named beeps (the profiles in beep_profiles.py) on the beep voice, with the
// RGB LED flashing in time: blue for operations, red for errors. Core 0 only.
//
// The beep itself is synthesised by the mixer (see beep.h), so play() returns
// at once and the idle/access loops keep playing underneath.
#pragma once

#include <stdint.h>

#include "settings.h"

namespace beeps {

enum class Type {
    // Operations (blue)
    ChangePack,
    FactoryReset,
    VolumeMode,
    BalanceMode,
    // Errors (red)
    NoSdCard,
    NoPacksOnSdCard,
    CorruptedPack,
    PackNotFound,
};

// Beeps follow the user's volume (see beepLevelFor() in beep.h) once this
// has been called.
void begin(const Settings& s);
void play(Type t);
bool active();

// Call every loop pass while a beep may be playing: drives the LED. Returns
// true once, on the pass where a beep has just ended and the LED is free
// again (so the caller can restore the activity colour).
bool service();

// Block until the current beep has finished, calling `idle` (e.g. to feed
// the watchdog) while waiting.
void waitDone(void (*idle)());

// After a fatal error: is the red LED on now? Repeats the error's beep
// pattern every RGB_LED_ERROR_REPEAT_S, as _halt_on_boot_error() does.
bool errorLedOn(Type t, uint32_t nowMs);

} // namespace beeps
