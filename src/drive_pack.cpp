#include "drive_pack.h"

#include <ArduinoJson.h>
#include <string.h>

#include "wav.h"

bool parseDriveJson(const char* json, DrivePack& out, const char*& why) {
    out = DrivePack{};
    why = "";
    JsonDocument doc;
    if (deserializeJson(doc, json)) {
        why = "not valid JSON";
        return false;
    }
    if (doc["preset"].is<const char*>() && !drivePreset(doc["preset"].as<const char*>(), out.params)) {
        why = "unknown preset";
        return false;
    }
    if (doc["sampleRate"].is<uint32_t>()) {
        out.sampleRate = doc["sampleRate"];
        if (!wavRateSupported(out.sampleRate)) {
            why = "unsupported sampleRate";
            return false;
        }
    }
    if (doc["name"].is<const char*>()) strncpy(out.name, doc["name"].as<const char*>(), sizeof(out.name) - 1);
    for (JsonPairConst kv : doc["params"].as<JsonObjectConst>()) {
        const char* key = kv.key().c_str();
        ParamResult r;
        if (kv.value().is<const char*>()) r = setDriveChoice(out.params, key, kv.value().as<const char*>());
        else if (kv.value().is<bool>()) r = setDriveParam(out.params, key, kv.value().as<bool>() ? 1.0f : 0.0f);
        else if (kv.value().is<float>()) r = setDriveParam(out.params, key, kv.value().as<float>());
        else r = ParamResult::BadValue;
        if (r == ParamResult::UnknownKey) out.unknownKeys++;
        else if (r == ParamResult::BadValue) out.badValues++;
    }
    return true;
}
