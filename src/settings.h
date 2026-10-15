// Runtime settings: defaults overridden by /settings.json on the SD card
// (same keys as the CircuitPython firmware, so existing cards keep working).
#pragma once

#include <stdint.h>

struct Settings {
    bool simulationMode = false;
    bool playSpinup = true;
    bool playSpindown = true;
    bool playIdle = true;
    bool powerDetection = true;
    uint32_t accessHoldMs = 100;
    float volume = 0.5f;
    float balance = 0.5f;
    bool activityLedEnabled = true;
    uint8_t activityLedColour[3] = {0, 255, 0};
    float rgbBrightness = 0.5f;
    char samplePack[64] = {0};
    bool randomSamplePack = false;   // RANDOM_SAMPLE_PACK: overrides SAMPLE_PACK
    bool alwaysPlayJingle = false;   // ALWAYS_PLAY_JINGLE (a settings.py constant in Python)
};

// Parse JSON text into `s` (unknown keys ignored, bad values keep defaults).
// Returns false if the text is not valid JSON.
bool parseSettingsJson(const char* json, Settings& s);
void printSettings(const Settings& s);
