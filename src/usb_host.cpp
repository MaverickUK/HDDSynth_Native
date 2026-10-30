#include "usb_host.h"

#include <Arduino.h>

#include "console.h"

namespace usbhost {

namespace {

constexpr uint32_t APP_ACTIVE_MS = 3000;

LinkWatch activity;
LinkState latest;
HeadFeed headFeed;
uint32_t lastSentenceMs = 0;
bool everSpoke = false;
bool wasPresent = false;
uint32_t valid = 0;
uint32_t rejected = 0;

void noteValid() {
    lastSentenceMs = millis();
    everSpoke = true;
    ++valid;
}

} // namespace

bool handleLine(const char* line, size_t len, HostCommand& cmd) {
    switch (parseHostCommand(line, len, cmd)) {
        case HostParse::Command:
            noteValid();
            return true;
        case HostParse::Invalid:
            ++rejected;
            return false;
        case HostParse::NotCommand:
            break;
    }
    HeadTarget h;
    if (parseHeadSentence(line, len, h)) {
        noteValid();
        headFeed.got(h, millis());
        return false;
    }
    LinkState s;
    if (!parseLinkSentence(line, len, s)) {
        ++rejected;
        return false;
    }
    noteValid();
    latest = s;
    activity.seen(millis());
    if (!wasPresent) {
        wasPresent = true;
        Console.println("[usb] companion app sending disk activity");
    }
    return false;
}

bool present() {
    const bool p = activity.present(millis());
    if (wasPresent && !p) {
        wasPresent = false;
        Console.println("[usb] companion app's disk activity stopped");
    }
    return p;
}

LinkState state() { return latest; }
const HeadFeed& heads() { return headFeed; }

bool appActive() { return everSpoke && millis() - lastSentenceMs < APP_ACTIVE_MS; }

void send(const char* body) {
    char buf[128];  // replies may be longer than the 80 the device accepts (pack names)
    const size_t n = formatSentence(buf, sizeof(buf), body);
    if (!n) return;
    // A burst (the pack list) can outrun the USB transmit buffer: wait for
    // room, but only briefly, so a host that has stopped reading can't stall
    // core 0. The sentence is dropped then; the app asks again.
    const uint32_t t0 = millis();
    while (Serial.availableForWrite() < static_cast<int>(n)) {
        if (millis() - t0 > 50) return;
        delay(1);
    }
    Serial.write(buf, n);
}

void printStats() {
    if (!everSpoke) return;
    Console.printf("[stats] usb app: %s activity=%s valid=%lu rejected=%lu\n", appActive() ? "connected" : "gone",
                   present() ? (latest.busy ? "busy" : "idle") : "none", static_cast<unsigned long>(valid),
                   static_cast<unsigned long>(rejected));
}

} // namespace usbhost
