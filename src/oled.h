// The optional 128x64 OLED (SSD1306, I2C) on header J6. Core 0 only.
//
// OLED_MODE picks the main screen: "activity" (a graph of HDD activity, one
// column per OLED_SAMPLE_MS, under the pack name), "supercap" (the same for
// the supercap voltage) or "debug" (the console output). Whatever the mode,
// turning the volume or balance shows that level for 4 s after the last
// change. Drawing lives in oled_screens.*; this file is the I2C driver and
// the logic of what to show when.
#pragma once

#include "settings.h"

namespace oled {

// At boot, after settings.json is read: if OLED_ENABLED, starts I2C on J6
// and the display. Logs and stays off if no display answers.
void begin(const Settings& s);

// The pack whose name the activity screen shows.
void setPack(const char* name);

// Call every loop pass while running: samples, and redraws when needed.
void service();

// Power loss: switch the panel off (it draws ~10-20 mA lit) and stop.
void off();

} // namespace oled
