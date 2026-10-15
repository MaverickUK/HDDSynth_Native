// Front-panel controls. Core 0 only.
//
// Rotary encoder: turning adjusts volume or balance; pressing it toggles
// which one (as rotary_enc.py in the CircuitPython firmware).
// Action button: short press = next sample pack, long press (held 3 s) =
// factory reset (as action_button.py). Those are returned to the caller,
// which owns the packs.
#pragma once

#include "settings.h"

namespace controls {

enum class Mode { Volume, Balance };
enum class Action { None, NextPack, FactoryReset };

// Configure pins and the encoder interrupts. `s` holds the live volume and
// balance, and must outlive the controls.
void begin(Settings& s);

// Call from loop(): applies encoder turns and the encoder button to the
// settings, and returns what the action button asks for.
Action poll();

Mode mode();

// Shared with the serial +/- commands: move volume or balance by `steps`
// encoder detents, clamped to 0..1. Returns true if the value changed.
bool adjustVolume(int steps);
bool adjustBalance(int steps);

} // namespace controls
