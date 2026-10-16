// Saved volume/balance (SAVE_VOLUME_BALANCE). Pure C++ (no Arduino),
// host-tested.
//
// Kept on the SD card, not in flash: a flash write pauses core 1 for ~40 ms,
// which could glitch the idle/access loops that are playing whenever the
// encoder is turned. An SD sector write takes a few ms on core 0 and the
// stream rings hold ~1 s, so it can't be heard.
//
// Two 512-byte slots are written alternately, each with a sequence number
// and a CRC. The newest valid one wins, so if power dies mid-write the
// previous value survives.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace levels {

constexpr size_t SECTOR_BYTES = 512;
constexpr uint8_t NOT_SAVED = 0xFF;  // a percent value that was never saved, or was cleared

struct Record {
    uint32_t seq = 0;
    uint8_t volumePct = NOT_SAVED;   // 0-100
    uint8_t balancePct = NOT_SAVED;  // 0-100
};

void encode(const Record& r, uint8_t* sector);
bool decode(const uint8_t* sector, Record& r);

// The newest valid record of the two slots; false if neither is valid.
bool newest(const uint8_t* slotA, const uint8_t* slotB, Record& r);

// Debounce: report when the values have been stable for `debounceMs` and
// differ from what was last saved. Feed it the current values every loop.
class Saver {
public:
    explicit Saver(uint32_t debounceMs) : debounceMs_(debounceMs) {}

    // Treat these values as already saved (e.g. what boot applied).
    void baseline(uint8_t volumePct, uint8_t balancePct) {
        savedVol_ = seenVol_ = volumePct;
        savedBal_ = seenBal_ = balancePct;
    }

    // True when the caller should write now.
    bool update(uint8_t volumePct, uint8_t balancePct, uint32_t nowMs) {
        if (volumePct != seenVol_ || balancePct != seenBal_) {
            seenVol_ = volumePct;
            seenBal_ = balancePct;
            changedMs_ = nowMs;
        }
        const bool unsaved = seenVol_ != savedVol_ || seenBal_ != savedBal_;
        return unsaved && nowMs - changedMs_ >= debounceMs_;
    }

    void saved() {
        savedVol_ = seenVol_;
        savedBal_ = seenBal_;
    }

    // The write failed (e.g. SD card out): try again after another debounce.
    void failed(uint32_t nowMs) { changedMs_ = nowMs; }

private:
    uint32_t debounceMs_;
    uint8_t savedVol_ = NOT_SAVED, savedBal_ = NOT_SAVED;
    uint8_t seenVol_ = NOT_SAVED, seenBal_ = NOT_SAVED;
    uint32_t changedMs_ = 0;
};

} // namespace levels
