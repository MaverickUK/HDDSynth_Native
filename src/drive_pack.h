// A modelled pack's /samples/<pack>/drive.json: the drive the engine
// synthesises in place of the pack's WAVs. The format is what the browser
// prototype (tools/drive-lab) exports, grouped as its cards:
//
//   { "format": "hddsynth-drive", "version": 2, "name": "1997 voice coil",
//     "preset": "voice97", "sampleRate": 32000,
//     "spindle": { "rpm": 5400, ... }, "actuator": { "type": "voice", ... },
//     ..., "mixer": { "spindle": 1, ..., "master": 0.8 } }
//
// Every key is optional: "preset" (default voice97) gives the starting
// parameters and the groups override any of them. Groups and keys are in
// drive_params.h and the README. Version 1 (one flat "params" object) is
// refused.
#pragma once

#include <stdint.h>

#include "drive_params.h"

struct DrivePack {
    DriveParams params;
    uint32_t sampleRate = 32000;  // the output rate while the pack is loaded
    char name[48] = {0};
    int unknownKeys = 0;          // keys this firmware doesn't know (logged)
    int badValues = 0;            // known keys with an unusable value (kept at the preset's)
};

// Parse drive.json text. Returns false if it isn't valid JSON, or names an
// unknown preset or an unsupported sample rate (`why` says which).
bool parseDriveJson(const char* json, DrivePack& out, const char*& why);
