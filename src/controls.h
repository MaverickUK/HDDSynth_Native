// Front-panel controls. Core 0 only.
//
// Rotary encoder: turning adjusts volume or balance; pressing it toggles
// which one (as rotary_enc.py in the CircuitPython firmware).
#pragma once

#include "settings.h"

namespace controls {

enum class Mode { Volume, Balance };

// Configure pins and the encoder interrupts. `s` holds the live volume and
// balance, and must outlive the controls.
void begin(Settings& s);

// Call from loop(): applies encoder turns and button presses to the settings.
// Returns true if volume or balance changed.
bool poll();

Mode mode();

// Shared with the serial +/- commands: move volume or balance by `steps`
// encoder detents, clamped to 0..1. Returns true if the value changed.
bool adjustVolume(int steps);
bool adjustBalance(int steps);

} // namespace controls
