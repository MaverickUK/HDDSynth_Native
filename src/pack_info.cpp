#include "pack_info.h"

#include <ArduinoJson.h>
#include <string.h>

#include "json_colour.h"

bool parsePackJson(const char* json, PackInfo& p) {
    p = PackInfo{};
    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;
    if (doc["NAME"].is<const char*>()) strncpy(p.name, doc["NAME"].as<const char*>(), sizeof(p.name) - 1);
    if (doc["AUTHOR"].is<const char*>()) strncpy(p.author, doc["AUTHOR"].as<const char*>(), sizeof(p.author) - 1);
    if (doc["GAIN"].is<float>()) {
        const float g = doc["GAIN"];
        p.gain = g < 0.0f ? 0.0f : (g > 2.0f ? 2.0f : g);
    }
    p.hasLedColour = parseColour(doc["ACTIVITY_LED_COLOUR"], p.ledColour);
    if (doc["BALANCE_DEFAULT"].is<float>()) {
        const float b = doc["BALANCE_DEFAULT"];
        p.balance = b < 0.0f ? 0.0f : (b > 1.0f ? 1.0f : b);
        p.hasBalance = true;
    }
    return true;
}
