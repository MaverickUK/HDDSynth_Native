// External power monitoring: TPS25921A /FLT (interrupt) + supercap voltage.
#pragma once

#include <stdint.h>

namespace power {

void begin(bool detectionEnabled);
// True once FLT has been low for cfg::POWER_LOSS_CONFIRM_MS. Latches.
bool lost();
// Force a simulated power loss (serial 'p' command).
void simulateLoss();
bool fltHigh();
float supercapVolts();
int supercapMillivolts();   // for logging (printf has no float support)
// Block until power has genuinely returned (FLT high AND supercap rising).
// Calls `idle` regularly so the caller can feed the watchdog.
void waitForRestore(void (*idle)());

} // namespace power
