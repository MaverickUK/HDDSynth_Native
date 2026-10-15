// Small board I/O: amplifier enable, LEDs and the HDD activity input.
#pragma once

#include <stdint.h>

#include "settings.h"

namespace io {

void begin(const Settings& s);

void ampOn();
void ampOff();

void rgb(uint8_t r, uint8_t g, uint8_t b);   // 0-255, master brightness applied
void rgbOff();
void hddLed(bool on);

// Activity with fast attack / slow release (ACCESS_HOLD_TIME_MS), or
// random simulated activity when SIMULATION_MODE is on.
bool activity();
void setSimulation(bool on);
bool simulation();

} // namespace io
