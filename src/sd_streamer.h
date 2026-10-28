// SD card access. Runs on core 0 only; it is the single owner of the card.
// Streams WAV sample data into each voice's ring buffer. If reads keep
// failing the card is marked down; pump() then looks for it every
// SD_REMOUNT_INTERVAL_MS without blocking, re-initialises it and reopens the
// files in place, so playback resumes where it left off.
#pragma once

#include <stdint.h>

#include "audio_engine.h"
#include "wav.h"

namespace sd {

struct Stream {
    VoiceId voice = VOICE_COUNT;
    SampleRing* ring = nullptr;
    char path[96] = {0};
    WavInfo info;
    uint32_t pos = 0;       // bytes consumed from the data chunk
    bool loop = false;
    bool open = false;      // file open and header valid
    bool feeding = false;   // producer still pushing into the ring
};

struct Stats {
    uint32_t chunksRead = 0;
    uint32_t readErrors = 0;     // failed reads (each retry counts)
    uint32_t recoveries = 0;     // card remounted after being down
    uint32_t failedRecoveries = 0;  // a card answered but would not mount
    uint32_t rescues = 0;        // the probe found the card left mid-transfer by a reset, and stopped it
    uint32_t maxReadMicros = 0;  // worst single chunk read since last reset
};

bool begin();                       // init SPI1 + card + filesystem; retries for cfg::SD_BOOT_RETRY_MS
bool present();                     // a card answers (quick probe; doesn't mount)
bool mountsNow();                   // SdFat's full init works (up to 2 s; leaves it unmounted)
bool cardOk();                      // false while the card is down

// Open a WAV for streaming into `ring` for `voice`. Validates the format.
bool openStream(Stream& s, const char* path, VoiceId voice, SampleRing* ring, bool loop);
// Rewind and fill the ring before playback. The voice must be idle.
// `startByte` (rounded down to a whole sample) starts it part-way through.
bool prime(Stream& s, uint32_t startByte = 0);
// Top up every feeding stream. Call often from loop(); never blocks for long.
void pump();
void closeStream(Stream& s);

// Load a whole WAV into a heap buffer (for spindown/beeps). Fails cleanly if
// it would leave less than cfg::HEAP_RESERVE_BYTES free.
bool loadToRam(const char* path, int16_t*& data, uint32_t& samples);

// Packs are the directories in /samples, sorted case-insensitively.
bool packExists(const char* name);  // false for ""
// Read a WAV's header without opening it for streaming. False if it is
// missing or not a WAV (the format may still be unplayable: check it).
bool wavInfo(const char* path, WavInfo& info);
bool fileExists(const char* path);
// Find a pack: `preferred` if it exists, else the first directory in /samples.
bool choosePack(const char* preferred, char* out, size_t outLen);
// Position of `name` in the sorted list, or -1.
int packIndex(const char* name);
// The pack at `index` (wrapping), and how many packs there are.
bool packByIndex(int index, char* out, size_t outLen, int& count);

// Raw sector access, for logs that must survive power loss (see powerlog).
// reserveFile creates `path` as one contiguous run of `bytes` (or reuses it)
// and returns its sectors, which can then be written without touching any
// filesystem metadata.
bool reserveFile(const char* path, uint32_t bytes, uint32_t& firstSector, uint32_t& sectors);
bool readSector(uint32_t sector, uint8_t* dst);
bool writeSector(uint32_t sector, const uint8_t* src);
// Whole-card access for USB drive mode (usb_drive.*): nothing else may use
// the card while it's shared.
uint32_t cardSectors();
bool readSectors(uint32_t sector, uint8_t* dst, size_t count);
bool writeSectors(uint32_t sector, const uint8_t* src, size_t count);
// End any multi-block read or write SdFat has left open, so the card is idle.
// Call before a reboot: the card stays powered through it, and one left
// mid-write ignores the next boot's CMD0 until its power is cut.
void finishTransfers();

// Print a directory's entries (name, size) to Serial, for the 'd' command.
void listDir(const char* path);

bool readTextFile(const char* path, char* buf, size_t bufLen, size_t& outLen);

Stats& stats();

} // namespace sd
