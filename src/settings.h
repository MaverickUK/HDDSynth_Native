// Runtime settings: defaults overridden by /settings.json on the SD card
// (same keys as the CircuitPython firmware, so existing cards keep working).
#pragma once

#include <stdint.h>

enum class OledMode : uint8_t { Activity, Supercap, Debug };

struct Settings {
    bool simulationMode = false;
    bool playSpinup = true;
    bool playSpindown = true;
    bool playIdle = true;
    bool powerDetection = true;
    uint32_t accessHoldMs = 100;
    float volume = 0.5f;
    float balance = 0.5f;
    bool volumeFromConfig = false;   // VOLUME_DEFAULT was in settings.json (wins over a saved value)
    bool balanceFromConfig = false;  // BALANCE_DEFAULT likewise
    bool saveVolumeBalance = false;  // SAVE_VOLUME_BALANCE: remember manual changes (on the SD card)
    bool activityLedEnabled = true;
    uint8_t activityLedColour[3] = {0, 255, 0};
    float rgbBrightness = 0.5f;
    char samplePack[64] = {0};
    bool randomSamplePack = false;   // RANDOM_SAMPLE_PACK: overrides SAMPLE_PACK
    bool alwaysPlayJingle = false;   // ALWAYS_PLAY_JINGLE (a settings.py constant in Python)
    bool demoMode = false;           // DEMO_MODE: cycle through every pack (see main.cpp)
    bool oledEnabled = false;        // OLED_ENABLED: the 128x64 display on J6
    OledMode oledMode = OledMode::Activity;  // OLED_MODE: "activity", "supercap" or "debug"
    uint32_t oledSampleMs = 1000;    // OLED_SAMPLE_MS: time per graph column
    uint32_t standbyMinutes = 0;     // STANDBY_MINUTES: spin down after this long without activity (0 = never)
};

// Parse JSON text into `s` (unknown keys ignored, bad values keep defaults).
// Returns false if the text is not valid JSON.
bool parseSettingsJson(const char* json, Settings& s);
void printSettings(const Settings& s);
