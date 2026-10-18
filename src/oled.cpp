#include "oled.h"

#include <Arduino.h>
#include <Wire.h>
#include <string.h>

#include "canvas.h"
#include "config.h"
#include "console.h"
#include "io.h"
#include "oled_model.h"
#include "oled_screens.h"
#include "power.h"

namespace oled {

namespace {

const Settings* settings = nullptr;
bool running = false;
Canvas* canvas = nullptr;  // 1 KB, allocated only when the OLED is enabled
char packName[64] = {0};

SampleHistory history;
DutyMeter activity(1000);
uint32_t nextSupercapMs = 0;
int supercapMv = 0;

Overlay overlay(cfg::OLED_OVERLAY_MS);
Overlay::Kind shownOverlay = Overlay::Kind::None;
int lastVolumePct = -1, lastBalancePct = -1;
uint32_t lastConsoleVersion = 0;
OledMode shownMode = OledMode::Activity;  // to notice a mode change (serial 'o')
bool dirty = true;
uint32_t lastFrameMs = 0;

// ---- SSD1306 over I2C ----
// Every transfer starts with a control byte: 0x00 = commands follow, 0x40 =
// display data follows.

bool command(const uint8_t* cmds, size_t n) {
    Wire.beginTransmission(cfg::OLED_I2C_ADDR);
    Wire.write(0x00);
    Wire.write(cmds, n);
    return Wire.endTransmission() == 0;
}

bool initPanel() {
    static const uint8_t init[] = {
        0xAE,        // display off while configuring
        0xD5, 0x80,  // clock divide / oscillator: the usual default
        0xA8, 0x3F,  // multiplex: 64 rows
        0xD3, 0x00,  // no vertical offset
        0x40,        // start line 0
        0x8D, 0x14,  // charge pump on (the module has no external VCC)
        0x20, 0x00,  // horizontal addressing: a frame is sent in one go
        0xA1,        // columns mirrored and
        0xC8,        // rows scanned bottom-up: so (0,0) is top-left on the module
        0xDA, 0x12,  // COM pin layout for 128x64
        0x81, 0xCF,  // contrast
        0xD9, 0xF1,  // pre-charge period
        0xDB, 0x40,  // VCOMH level
        0xA4,        // show RAM contents
        0xA6,        // not inverted
        0x2E,        // no scrolling
        0xAF,        // display on
    };
    return command(init, sizeof(init));
}

void sendFrame() {
    static const uint8_t window[] = {0x21, 0, 127, 0x22, 0, 7};  // whole screen
    command(window, sizeof(window));
    const uint8_t* p = canvas->data();
    constexpr size_t CHUNK = 128;  // fits the Wire buffer with the control byte
    for (size_t off = 0; off < Canvas::BYTES; off += CHUNK) {
        Wire.beginTransmission(cfg::OLED_I2C_ADDR);
        Wire.write(0x40);
        Wire.write(p + off, CHUNK);
        Wire.endTransmission();
    }
}

// ---- what to show ----

int toPct(float v) { return static_cast<int>(lroundf(v * 100.0f)); }

void render(Overlay::Kind kind) {
    if (kind == Overlay::Kind::Volume) {
        drawVolume(*canvas, toPct(settings->volume));
    } else if (kind == Overlay::Kind::Balance) {
        drawBalance(*canvas, toPct(settings->balance));
    } else if (settings->oledMode == OledMode::Debug) {
        drawConsole(*canvas, Console.text());
    } else if (settings->oledMode == OledMode::Supercap) {
        char title[24];
        snprintf(title, sizeof(title), "Supercap %d.%02dV", supercapMv / 1000, (supercapMv % 1000) / 10);
        drawGraph(*canvas, title, history);
    } else {
        drawGraph(*canvas, packName[0] ? packName : "HDD Synth", history);
    }
}

void sample(uint32_t now) {
    if (settings->oledMode == OledMode::Activity) {
        uint8_t v;
        if (activity.update(io::activity(), now, v)) {
            history.push(v);
            dirty = true;
        }
    } else if (settings->oledMode == OledMode::Supercap) {
        if (static_cast<int32_t>(now - nextSupercapMs) >= 0) {
            nextSupercapMs = now + settings->oledSampleMs;
            supercapMv = power::supercapMillivolts();
            const int v = supercapMv * 255 / cfg::OLED_SUPERCAP_FULL_MV;
            history.push(static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)));
            dirty = true;
        }
    } else if (Console.text().version() != lastConsoleVersion) {
        lastConsoleVersion = Console.text().version();
        dirty = true;
    }
}

} // namespace

void begin(const Settings& s) {
    settings = &s;
    if (!s.oledEnabled) return;
    Wire.setSDA(cfg::PIN_OLED_SDA);
    Wire.setSCL(cfg::PIN_OLED_SCL);
    Wire.setClock(cfg::OLED_I2C_HZ);
    Wire.begin();
    if (!initPanel()) {
        Console.println("[oled] no display answering on J6 (I2C 0x3C); OLED off");
        Wire.end();
        return;
    }
    canvas = new Canvas;
    activity = DutyMeter(s.oledSampleMs);
    shownMode = s.oledMode;
    lastVolumePct = toPct(s.volume);
    lastBalancePct = toPct(s.balance);
    running = true;
    static const char* const names[] = {"activity", "supercap", "debug"};
    Console.printf("[oled] on, mode %s, %lu ms per column\n", names[static_cast<int>(s.oledMode)],
                   static_cast<unsigned long>(s.oledSampleMs));
}

void setPack(const char* name) {
    strncpy(packName, name, sizeof(packName) - 1);
    dirty = true;
}

void service() {
    if (!running) return;
    const uint32_t now = millis();
    if (settings->oledMode != shownMode) {
        // Mode changed: the other graph's history means nothing here.
        shownMode = settings->oledMode;
        history = SampleHistory{};
        activity = DutyMeter(settings->oledSampleMs);
        nextSupercapMs = now;
        dirty = true;
    }
    sample(now);

    // Volume or balance changed (encoder or serial): show it for a while.
    const int vol = toPct(settings->volume), bal = toPct(settings->balance);
    if (vol != lastVolumePct) overlay.show(Overlay::Kind::Volume, now);
    if (bal != lastBalancePct) overlay.show(Overlay::Kind::Balance, now);
    if (vol != lastVolumePct || bal != lastBalancePct) dirty = true;
    lastVolumePct = vol;
    lastBalancePct = bal;
    const Overlay::Kind kind = overlay.current(now);
    if (kind != shownOverlay) {
        shownOverlay = kind;
        dirty = true;
    }

    // Each frame blocks core 0 for ~25 ms, so limit the rate.
    const uint32_t minGap = kind == Overlay::Kind::None && settings->oledMode == OledMode::Debug
                                ? cfg::OLED_DEBUG_FRAME_MS
                                : cfg::OLED_FRAME_MS;
    if (!dirty || now - lastFrameMs < minGap) return;
    dirty = false;
    lastFrameMs = now;
    render(kind);
    sendFrame();
}

void off() {
    if (!running) return;
    static const uint8_t displayOff[] = {0xAE};
    command(displayOff, sizeof(displayOff));
    running = false;
}

} // namespace oled
