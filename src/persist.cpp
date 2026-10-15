#include "persist.h"

#include <Arduino.h>
#include <EEPROM.h>
#include <string.h>

namespace persist {

namespace {

PersistedState live;
PersistedState onFlash;  // what was last loaded or written, to skip identical writes

bool same(const PersistedState& a, const PersistedState& b) {
    return strcmp(a.desiredPack, b.desiredPack) == 0 && a.jinglePlayed == b.jinglePlayed;
}

} // namespace

void begin() {
    EEPROM.begin(persistfmt::RECORD_BYTES);
    if (persistfmt::decode(EEPROM.getConstDataPtr(), live)) {
        Serial.printf("[persist] pack '%s', jingle %s\n", live.desiredPack, live.jinglePlayed ? "played" : "not played");
    } else {
        Serial.println("[persist] nothing saved yet, using defaults");
    }
    onFlash = live;
}

PersistedState& state() { return live; }

uint32_t commit() {
    if (same(live, onFlash)) return 0;
    const uint32_t t0 = millis();
    persistfmt::encode(live, EEPROM.getDataPtr());
    EEPROM.commit();
    onFlash = live;
    return millis() - t0;
}

} // namespace persist
