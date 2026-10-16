#include "settings.h"

#include <Arduino.h>
#include <ArduinoJson.h>

namespace {

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

bool parseColour(JsonVariantConst v, uint8_t out[3]) {
    if (v.is<const char*>()) {
        const char* s = v.as<const char*>();
        if (*s == '#') ++s;
        if (strlen(s) != 6) return false;
        char* end = nullptr;
        const unsigned long rgb = strtoul(s, &end, 16);
        if (*end) return false;
        out[0] = (rgb >> 16) & 0xFF;
        out[1] = (rgb >> 8) & 0xFF;
        out[2] = rgb & 0xFF;
        return true;
    }
    if (v.is<JsonArrayConst>() && v.size() == 3) {
        for (size_t i = 0; i < 3; ++i) {
            const int c = v[i].as<int>();
            out[i] = static_cast<uint8_t>(c < 0 ? 0 : (c > 255 ? 255 : c));
        }
        return true;
    }
    return false;
}

} // namespace

bool parseSettingsJson(const char* json, Settings& s) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;

    if (doc["SIMULATION_MODE"].is<bool>()) s.simulationMode = doc["SIMULATION_MODE"];
    if (doc["PLAY_SPINUP"].is<bool>()) s.playSpinup = doc["PLAY_SPINUP"];
    if (doc["PLAY_SPINDOWN"].is<bool>()) s.playSpindown = doc["PLAY_SPINDOWN"];
    if (doc["PLAY_IDLE"].is<bool>()) s.playIdle = doc["PLAY_IDLE"];
    if (doc["POWER_DETECTION"].is<bool>()) s.powerDetection = doc["POWER_DETECTION"];
    if (doc["ACCESS_HOLD_TIME_MS"].is<int>()) s.accessHoldMs = doc["ACCESS_HOLD_TIME_MS"].as<int>();
    if (doc["VOLUME_DEFAULT"].is<float>()) {
        s.volume = clamp01(doc["VOLUME_DEFAULT"]);
        s.volumeFromConfig = true;
    }
    if (doc["BALANCE_DEFAULT"].is<float>()) {
        s.balance = clamp01(doc["BALANCE_DEFAULT"]);
        s.balanceFromConfig = true;
    }
    if (doc["SAVE_VOLUME_BALANCE"].is<bool>()) s.saveVolumeBalance = doc["SAVE_VOLUME_BALANCE"];
    if (doc["ACTIVITY_LED_ENABLED"].is<bool>()) s.activityLedEnabled = doc["ACTIVITY_LED_ENABLED"];
    if (doc["RGB_LED_BRIGHTNESS"].is<float>()) s.rgbBrightness = clamp01(doc["RGB_LED_BRIGHTNESS"]);
    parseColour(doc["ACTIVITY_LED_COLOUR"], s.activityLedColour);
    if (doc["RANDOM_SAMPLE_PACK"].is<bool>()) s.randomSamplePack = doc["RANDOM_SAMPLE_PACK"];
    if (doc["ALWAYS_PLAY_JINGLE"].is<bool>()) s.alwaysPlayJingle = doc["ALWAYS_PLAY_JINGLE"];
    if (doc["DEMO_MODE"].is<bool>()) s.demoMode = doc["DEMO_MODE"];
    if (doc["SAMPLE_PACK"].is<const char*>()) {
        strncpy(s.samplePack, doc["SAMPLE_PACK"].as<const char*>(), sizeof(s.samplePack) - 1);
    }
    return true;
}

void printSettings(const Settings& s) {
    Serial.printf("[settings] pack='%s' volume=%d%% balance=%d%% sim=%d spinup=%d spindown=%d idle=%d "
                  "hold=%lums power_detect=%d random_pack=%d always_jingle=%d demo=%d save_levels=%d\n",
                  s.samplePack, static_cast<int>(s.volume * 100 + 0.5f), static_cast<int>(s.balance * 100 + 0.5f), s.simulationMode, s.playSpinup, s.playSpindown,
                  s.playIdle, static_cast<unsigned long>(s.accessHoldMs), s.powerDetection, s.randomSamplePack,
                  s.alwaysPlayJingle, s.demoMode, s.saveVolumeBalance);
}
