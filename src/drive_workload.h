// Which virtual workload the modelled drive runs, from the activity input.
// Pure C++ (no Arduino), host-tested.
#pragma once

#include <stdint.h>

#include "drive_engine.h"
#include "link_parser.h"

class WorkloadChooser {
public:
    // A plain HDD LED can't say what kind of access it is: activity that
    // stays on this long is taken for a file copy (sequential).
    static constexpr uint32_t LONG_BURST_MS = 1500;

    // `cls` comes from the HDDSynthPro link or the companion app when
    // `classKnown`; it can be Idle for a moment at a burst's edges, so the
    // last busy class sticks.
    DriveWorkload choose(bool active, ActivityClass cls, bool classKnown, uint32_t nowMs) {
        if (cls != ActivityClass::Idle) lastClass_ = cls;
        if (!active) {
            busy_ = false;
            return DriveWorkload::Idle;
        }
        if (!busy_) {
            busy_ = true;
            since_ = nowMs;
        }
        if (classKnown) return lastClass_ == ActivityClass::Sequential ? DriveWorkload::Sequential : DriveWorkload::Random;
        return nowMs - since_ >= LONG_BURST_MS ? DriveWorkload::Sequential : DriveWorkload::Random;
    }

private:
    bool busy_ = false;
    uint32_t since_ = 0;
    ActivityClass lastClass_ = ActivityClass::Random;
};
