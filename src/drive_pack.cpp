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
    if (doc["version"].is<int>() && doc["version"].as<int>() < 2) {
        why = "version 1 (flat) format: export it again from the Drive Lab";
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
    for (JsonPairConst top : doc.as<JsonObjectConst>()) {
        const char* group = top.key().c_str();
        if (!top.value().is<JsonObjectConst>()) {
            // The pack's own keys, above; any other value outside a group is unknown.
            static const char* const OWN[] = {"format", "version", "name", "preset", "sampleRate"};
            bool own = false;
            for (const char* k : OWN) own = own || !strcmp(group, k);
            if (!own) out.unknownKeys++;
            continue;
        }
        for (JsonPairConst kv : top.value().as<JsonObjectConst>()) {
            const char* key = kv.key().c_str();
            JsonVariantConst v = kv.value();
            ParamResult r;
            if (v.is<const char*>()) r = setDriveChoice(out.params, group, key, v.as<const char*>());
            else if (v.is<bool>()) r = setDriveParam(out.params, group, key, v.as<bool>() ? 1.0f : 0.0f);
            else if (v.is<float>()) r = setDriveParam(out.params, group, key, v.as<float>());
            else r = ParamResult::BadValue;
            if (r == ParamResult::UnknownKey) out.unknownKeys++;
            else if (r == ParamResult::BadValue) out.badValues++;
        }
    }
    return true;
}
