// Small board I/O: amplifier enable, LEDs and the HDD activity input.
#pragma once

#include <stdint.h>

#include "link_parser.h"
#include "settings.h"

namespace io {

void begin(const Settings& s);

void ampOn();
void ampOff();

void rgb(uint8_t r, uint8_t g, uint8_t b);   // 0-255, master brightness applied
void rgbOff();
void hddLed(bool on);
// Low-power wait: all LEDs off as plain digital outputs, PWM stopped.
void ledsOffForLowPower();

// Activity with fast attack / slow release (ACCESS_HOLD_TIME_MS), or
// random simulated activity when SIMULATION_MODE is on. While the
// HDDSynthPro link is present, its busy flag replaces the activity input.
bool activity();
// What kind of activity: from the link while present, Random without it.
// Simulation (with EXPERIMENTAL_HDDSYNTHPRO_SUPPORT on) mixes in longer
// sequential spells, so sequential.wav can be heard without HDDSynthPro.
ActivityClass activityClass();
void setSimulation(bool on);
bool simulation();

} // namespace io
