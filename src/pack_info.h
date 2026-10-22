// Optional per-pack settings in /samples/<pack>/pack.json, same key style as
// settings.json:
//
//   { "NAME": "IBM Ultrastar 36LZX", "AUTHOR": "PB", "GAIN": 1.4,
//     "ACTIVITY_LED_COLOUR": "#FF8800", "BALANCE_DEFAULT": 0.3 }
//
// Every key is optional; a missing file means all defaults.
#pragma once

#include <stdint.h>

struct PackInfo {
    char name[48] = {0};     // NAME: shown on the OLED instead of the folder name
    char author[48] = {0};   // AUTHOR: credited in the log
    float gain = 1.0f;       // GAIN: 0.0-2.0, scales the pack's sounds (not beeps or the jingle)
    bool hasLedColour = false;
    uint8_t ledColour[3] = {0, 0, 0};  // ACTIVITY_LED_COLOUR: overrides settings.json's
    bool hasBalance = false;
    float balance = 0.5f;    // BALANCE_DEFAULT: the pack's own starting balance
};

// Parse pack.json text into `p` (unknown keys ignored, bad values keep
// defaults). Returns false if the text is not valid JSON.
bool parsePackJson(const char* json, PackInfo& p);
