#include "saved_levels.h"

#include <Arduino.h>

#include "config.h"
#include "console.h"
#include "levels_format.h"
#include "sd_streamer.h"

namespace savedlevels {

namespace {

constexpr char PATH[] = "/state.bin";
constexpr uint32_t FILE_BYTES = 2 * levels::SECTOR_BYTES;  // slots A and B

Settings* settings = nullptr;
bool ready = false;  // saving enabled and the file is reserved
uint32_t firstSector = 0;
levels::Record current;  // what is on the card (or will be)
levels::Saver saver(cfg::SAVE_LEVELS_DEBOUNCE_MS);
uint8_t sectorBuf[levels::SECTOR_BYTES];

uint8_t toPct(float v) { return static_cast<uint8_t>(lroundf(v * 100.0f)); }

bool write(uint8_t volumePct, uint8_t balancePct) {
    levels::Record next = current;
    next.seq = current.seq + 1;
    next.volumePct = volumePct;
    next.balancePct = balancePct;
    levels::encode(next, sectorBuf);
    if (!sd::writeSector(firstSector + (next.seq & 1), sectorBuf)) return false;
    current = next;
    return true;
}

// Find (or create) /state.bin and read the newest record into `current`.
bool open() {
    uint32_t sectors = 0;
    if (!sd::reserveFile(PATH, FILE_BYTES, firstSector, sectors)) return false;
    uint8_t slotB[levels::SECTOR_BYTES];
    if (!sd::readSector(firstSector, sectorBuf)) memset(sectorBuf, 0, sizeof(sectorBuf));
    if (!sd::readSector(firstSector + 1, slotB)) memset(slotB, 0, sizeof(slotB));
    if (!levels::newest(sectorBuf, slotB, current)) current = levels::Record{};
    return true;
}

} // namespace

void begin(Settings& s) {
    settings = &s;
    ready = false;
    if (!s.saveVolumeBalance) return;
    if (!open()) {
        Console.println("[levels] cannot reserve /state.bin, not saving volume/balance");
        return;
    }
    ready = true;

    // settings.json wins over a saved value, key by key.
    if (current.volumePct != levels::NOT_SAVED && !s.volumeFromConfig) s.volume = current.volumePct / 100.0f;
    if (current.balancePct != levels::NOT_SAVED && !s.balanceFromConfig) s.balance = current.balancePct / 100.0f;
    Console.printf("[levels] saving on; volume %d%% (%s), balance %d%% (%s)\n", toPct(s.volume),
                  s.volumeFromConfig ? "settings.json" : current.volumePct != levels::NOT_SAVED ? "saved" : "default",
                  toPct(s.balance),
                  s.balanceFromConfig ? "settings.json" : current.balancePct != levels::NOT_SAVED ? "saved" : "default");
    saver.baseline(toPct(s.volume), toPct(s.balance));
}

void service() {
    if (!ready) return;
    const uint8_t v = toPct(settings->volume), b = toPct(settings->balance);
    const uint32_t now = millis();
    if (!saver.update(v, b, now)) return;
    if (write(v, b)) {
        saver.saved();
        Console.printf("[levels] saved volume %d%%, balance %d%%\n", v, b);
    } else {
        saver.failed(now);  // card out or busy: try again after another debounce
    }
}

void clear() {
    // Also when saving is off, so turning it back on later can't bring old
    // values back. Without an existing file there is nothing to clear.
    if (!ready && (!sd::fileExists(PATH) || !open())) return;
    if (write(levels::NOT_SAVED, levels::NOT_SAVED)) Console.println("[levels] saved volume/balance cleared");
}

} // namespace savedlevels
