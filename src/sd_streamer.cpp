#include "sd_streamer.h"

#include <Arduino.h>
#include <SPI.h>
#include <SdFat.h>
#include <hardware/gpio.h>

#include <memory>
#include <new>

#include "config.h"
#include "console.h"

namespace sd {

namespace {

SdFs card;
bool mounted = false;      // false after a failed read: the card is "down" until a remount works
uint32_t downSinceMs = 0;
uint32_t nextRemountMs = 0;
Stats st;

// Streams that are open, so recovery can reopen them. Small fixed table.
constexpr int MAX_STREAMS = 6;
Stream* registry[MAX_STREAMS] = {nullptr};
FsFile files[MAX_STREAMS];

int slotOf(const Stream& s) {
    for (int i = 0; i < MAX_STREAMS; ++i)
        if (registry[i] == &s) return i;
    return -1;
}

int claimSlot(Stream& s) {
    int i = slotOf(s);
    if (i >= 0) return i;
    for (i = 0; i < MAX_STREAMS; ++i) {
        if (!registry[i]) {
            registry[i] = &s;
            return i;
        }
    }
    return -1;
}

SdSpiConfig spiConfig() {
    return SdSpiConfig(cfg::PIN_SD_CS, DEDICATED_SPI, cfg::SD_SPI_HZ, &SPI1);
}

void setSpiPins() {
    SPI1.setRX(cfg::PIN_SD_MISO);
    SPI1.setSCK(cfg::PIN_SD_SCK);
    SPI1.setTX(cfg::PIN_SD_MOSI);
    // With no card in the slot MISO floats. The pull-up makes it read 0xFF
    // (no response), which is also what the SD spec recommends.
    gpio_pull_up(cfg::PIN_SD_MISO);
}

bool mount() {
    setSpiPins();
    mounted = card.begin(spiConfig());
    if (!mounted) {
        Console.printf("[sd] mount failed: error 0x%02X data 0x%02X\n",
                      card.sdErrorCode(), card.sdErrorData());
    }
    return mounted;
}

// Read the start of a WAV and parse its header. The data chunk has to begin
// within cfg::WAV_HEADER_BYTES: 4 KB leaves room for the metadata chunks
// (bext, LIST, markers) that editors put before the audio. The buffer is
// borrowed from the heap just for this, not kept: a permanent 4 KB would cut
// into the RAM a large spindown needs, and it's too big for core 0's stack.
WavError readWavInfo(FsFile& f, WavInfo& info) {
    std::unique_ptr<uint8_t[]> header(new (std::nothrow) uint8_t[cfg::WAV_HEADER_BYTES]);
    if (!header) {
        Console.println("[sd] no memory to read a WAV header");
        return WavError::Truncated;
    }
    f.seekSet(0);
    const int n = f.read(header.get(), cfg::WAV_HEADER_BYTES);
    return parseWavHeader(header.get(), n > 0 ? static_cast<size_t>(n) : 0, static_cast<uint32_t>(f.fileSize()), info);
}

bool seekTo(Stream& s, FsFile& f) {
    return f.seekSet(s.info.dataOffset + s.pos);
}

// Reopen every open stream at its current position (after a card re-init).
bool reopenAll() {
    for (int i = 0; i < MAX_STREAMS; ++i) {
        Stream* s = registry[i];
        if (!s || !s->open) continue;
        files[i].close();
        if (!files[i].open(s->path, O_RDONLY) || !seekTo(*s, files[i])) {
            Console.printf("[sd] reopen failed: %s\n", s->path);
            return false;
        }
    }
    return true;
}

uint8_t lastProbeReply = 0xFF;  // the last probe's CMD0 reply, for the "no SD card" log

// Send a command with a zero argument and return its R1 reply, read as
// SdFat does: skip one byte, then up to 9 fill bytes (bit 7 set).
uint8_t probeCommand(uint8_t cmd, uint8_t crc) {
    const uint8_t b[] = {static_cast<uint8_t>(0x40 | cmd), 0, 0, 0, 0, crc};
    for (uint8_t x : b) SPI1.transfer(x);
    SPI1.transfer(0xFF);
    uint8_t r1 = 0xFF;
    for (int i = 0; i < 9 && (r1 & 0x80); ++i) r1 = SPI1.transfer(0xFF);
    return r1;
}

// Quick check for a card: CMD0 (GO_IDLE_STATE) and its 0x01 reply. A few ms
// with or without a card. card.begin() instead waits up to 2 s for an absent
// card, longer than the watchdog allows, and power-loss detection would be
// blind for that time.
//
// A reset that doesn't power the card off (a reboot, the watchdog, RUN, a
// dip that only resets the RP2350) can leave the card mid-transfer: still
// streaming a multi-block read, or busy finishing a write. It then answers
// with data or busy bytes instead of 0x01, or (mid-write, waiting for the
// next data block) ignore CMD0 altogether until its power is cut. So if
// CMD0 fails, end any write (the stop token) and any read (CMD12), let the
// card finish, and try CMD0 a few more times. (CMD0 still goes first: it's
// the command a freshly powered card expects.) finishTransfers() before a
// planned reboot avoids all this; this covers unplanned resets.
bool cardResponds() {
    setSpiPins();
    pinMode(cfg::PIN_SD_CS, OUTPUT);
    digitalWrite(cfg::PIN_SD_CS, HIGH);
    SPI1.begin();
    SPI1.beginTransaction(SPISettings(cfg::SD_PROBE_HZ, MSBFIRST, SPI_MODE0));
    for (int i = 0; i < 10; ++i) SPI1.transfer(0xFF);  // 80 clocks with CS high: wake up
    digitalWrite(cfg::PIN_SD_CS, LOW);
    // CRCs are real: a card left in SPI mode by SdFat (USE_SD_CRC) checks them.
    lastProbeReply = probeCommand(0, 0x95);
    bool ok = lastProbeReply == 0x01;
    if (!ok) {
        auto waitNotBusy = [] { for (int i = 0; i < 2000 && SPI1.transfer(0xFF) != 0xFF; ++i) {} };  // up to ~40 ms
        waitNotBusy();
        SPI1.transfer(0xFD);     // stop token: ends a multi-block write; ignored otherwise
        SPI1.transfer(0xFF);
        waitNotBusy();
        probeCommand(12, 0x61);  // STOP_TRANSMISSION: ends a multi-block read; an idle card refuses it
        waitNotBusy();
        for (int attempt = 0; attempt < 8 && !ok; ++attempt) {
            for (int i = 0; i < 32; ++i) SPI1.transfer(0xFF);  // clock out whatever it's still sending
            lastProbeReply = probeCommand(0, 0x95);
            ok = lastProbeReply == 0x01;
        }
        if (ok) {
            st.rescues++;
            Console.println("[sd] card was left mid-transfer (by a reset); stopped it");
        }
    }
    digitalWrite(cfg::PIN_SD_CS, HIGH);
    SPI1.transfer(0xFF);
    SPI1.endTransaction();
    SPI1.end();
    return ok;
}

// After repeated read failures: close everything and mark the card down.
// pump() then looks for it every SD_REMOUNT_INTERVAL_MS without blocking, so
// the main loop keeps checking for power loss and feeding the watchdog.
void markDown() {
    for (int i = 0; i < MAX_STREAMS; ++i) files[i].close();
    card.end();
    mounted = false;
    downSinceMs = millis();
    nextRemountMs = downSinceMs;  // the first look is immediate
    Console.println("[sd] card down, playing from buffers");
}

void tryRemount() {
    const uint32_t now = millis();
    if (static_cast<int32_t>(now - nextRemountMs) < 0) return;
    nextRemountMs = now + cfg::SD_REMOUNT_INTERVAL_MS;
    if (!cardResponds()) return;  // still absent: try again later
    if (mount() && reopenAll()) {
        st.recoveries++;
        Console.printf("[sd] card back after %lu ms\n", static_cast<unsigned long>(millis() - downSinceMs));
        return;
    }
    // A card answered but would not mount or reopen. Leave it down and retry.
    st.failedRecoveries++;
    for (int i = 0; i < MAX_STREAMS; ++i) files[i].close();
    card.end();
    mounted = false;
}

// Read up to `bytes` of sample data, retrying a few times. If every attempt
// fails the card is marked down; pump() takes care of bringing it back.
int readData(Stream& s, int slot, uint8_t* dst, uint32_t bytes) {
    if (!mounted) return -1;
    const uint32_t firstTry = millis();
    for (int attempt = 0; attempt < cfg::SD_READ_RETRIES; ++attempt) {
        if (attempt > 0 && !seekTo(s, files[slot])) {
            st.readErrors++;
            continue;
        }
        const uint32_t t0 = micros();
        const int n = files[slot].read(dst, bytes);
        const uint32_t dt = micros() - t0;
        if (dt > st.maxReadMicros) st.maxReadMicros = dt;
        if (n == static_cast<int>(bytes)) {
            st.chunksRead++;
            return n;
        }
        st.readErrors++;
        Console.printf("[sd] read error on %s at %lu (got %d), attempt %d\n", s.path,
                      static_cast<unsigned long>(s.pos), n, attempt + 1);
    }
    Console.printf("[sd] giving up on reads after %lu ms\n", static_cast<unsigned long>(millis() - firstTry));
    markDown();
    return -1;
}

// Push one chunk into the stream's ring. Returns false if nothing more can be done now.
bool feedOnce(Stream& s, int slot) {
    static int16_t chunk[cfg::SD_CHUNK_SAMPLES];
    if (!s.feeding || s.ring->free() < cfg::SD_CHUNK_SAMPLES) return false;

    if (s.pos >= s.info.dataBytes) {
        if (s.loop) {
            s.pos = 0;
            if (!seekTo(s, files[slot])) {
                st.readErrors++;
                return false;
            }
        } else {
            s.feeding = false;
            audio::markEndOfStream(s.voice);
            return false;
        }
    }

    uint32_t bytes = s.info.dataBytes - s.pos;
    if (bytes > sizeof(chunk)) bytes = sizeof(chunk);
    const int n = readData(s, slot, reinterpret_cast<uint8_t*>(chunk), bytes);
    if (n <= 0) return false;  // the ring keeps playing what it has; retried next pump
    s.pos += static_cast<uint32_t>(n);
    s.ring->push(chunk, static_cast<size_t>(n) / sizeof(int16_t));
    return true;
}

} // namespace

bool begin() {
    // Some cards aren't ready the instant power comes up (and a PC's supply
    // rises more slowly than USB), so keep trying for a while. The quick
    // presence probe avoids card.begin()'s long timeout on an absent card.
    const uint32_t t0 = millis();
    int attempts = 0;
    do {
        ++attempts;
        if (cardResponds() && mount()) {
            if (attempts > 1) Console.printf("[sd] card ready after %d attempts\n", attempts);
            return true;
        }
        delay(100);
    } while (millis() - t0 < cfg::SD_BOOT_RETRY_MS);
    // Never answered the probe: say what it did answer, and give SdFat's own,
    // slower init (2 s of CMD0 retries) a last go before giving up.
    Console.printf("[sd] no answer to the probe after %d attempts (CMD0 reply 0x%02X; 0xFF = nothing)\n", attempts,
                   lastProbeReply);
    return mount();
}

bool present() { return cardResponds(); }

bool mountsNow() {
    const bool ok = mount();
    card.end();
    mounted = false;
    return ok;
}

bool cardOk() { return mounted; }

Stats& stats() { return st; }

bool openStream(Stream& s, const char* path, VoiceId voice, SampleRing* ring, bool loop) {
    closeStream(s);
    if (!mounted) return false;
    const int slot = claimSlot(s);
    if (slot < 0) return false;

    strncpy(s.path, path, sizeof(s.path) - 1);
    s.voice = voice;
    s.ring = ring;
    s.loop = loop;
    s.pos = 0;
    s.feeding = false;

    FsFile& f = files[slot];
    if (!f.open(path, O_RDONLY)) {
        Console.printf("[sd] cannot open %s\n", path);
        return false;
    }
    const WavError e = readWavInfo(f, s.info);
    if (e != WavError::Ok || !wavIsPlayable(s.info)) {
        Console.printf("[sd] %s: %s (fmt %u, %u ch, %lu Hz, %u bit)\n", path,
                      e == WavError::Ok ? "wrong format" : wavErrorName(e), s.info.format,
                      s.info.channels, static_cast<unsigned long>(s.info.sampleRate), s.info.bitsPerSample);
        f.close();
        return false;
    }
    s.open = true;
    return true;
}

bool prime(Stream& s, uint32_t startByte) {
    const int slot = slotOf(s);
    if (!mounted || slot < 0 || !s.open || !audio::isIdle(s.voice)) return false;

    s.ring->reset();
    audio::voice(s.voice).eof.store(false, std::memory_order_relaxed);
    s.pos = startByte < s.info.dataBytes ? startByte & ~1u : 0;
    if (!seekTo(s, files[slot])) return false;
    s.feeding = true;
    while (feedOnce(s, slot)) {
    }
    // Short non-looping samples may already be complete.
    if (!s.loop && s.pos >= s.info.dataBytes && s.feeding) {
        s.feeding = false;
        audio::markEndOfStream(s.voice);
    }
    return s.ring->size() > 0;
}

void pump() {
    // A few chunks per stream per call keeps loop() responsive; at 2 KB per
    // chunk that is far more than the 32 KB/s each voice consumes.
    constexpr int CHUNKS_PER_CALL = 4;
    if (!mounted) {
        tryRemount();
        if (!mounted) return;
    }
    for (int i = 0; i < MAX_STREAMS; ++i) {
        Stream* s = registry[i];
        if (!s || !s->open || !s->feeding) continue;
        for (int c = 0; c < CHUNKS_PER_CALL && feedOnce(*s, i); ++c) {
        }
    }
}

void closeStream(Stream& s) {
    const int slot = slotOf(s);
    if (slot >= 0) {
        files[slot].close();
        registry[slot] = nullptr;
    }
    s.open = false;
    s.feeding = false;
}

bool loadToRam(const char* path, int16_t*& data, uint32_t& samples) {
    data = nullptr;
    samples = 0;
    FsFile f;
    if (!f.open(path, O_RDONLY)) return false;
    WavInfo info;
    if (readWavInfo(f, info) != WavError::Ok || !wavIsPlayable(info)) {
        return false;
    }
    if (rp2040.getFreeHeap() < static_cast<int>(info.dataBytes + cfg::HEAP_RESERVE_BYTES)) {
        Console.printf("[sd] %s (%lu bytes) won't fit in RAM, free heap %d\n", path,
                      static_cast<unsigned long>(info.dataBytes), rp2040.getFreeHeap());
        return false;
    }
    auto* buf = static_cast<int16_t*>(malloc(info.dataBytes));
    if (!buf) return false;
    if (!f.seekSet(info.dataOffset) ||
        f.read(buf, info.dataBytes) != static_cast<int>(info.dataBytes)) {
        st.readErrors++;
        free(buf);
        return false;
    }
    data = buf;
    samples = info.dataBytes / sizeof(int16_t);
    return true;
}

// The pack directories in /samples, in case-insensitive order, found by
// rescanning rather than storing a list: it only runs at boot, and RAM is
// better spent keeping big spindowns in memory.
// Returns the name that sorts straight after `after` ("" = the first), or false.
bool nextPackName(const char* after, char* out, size_t outLen) {
    char prev[64];  // copy first: callers may pass the same buffer as after and out
    strncpy(prev, after, sizeof(prev) - 1);
    prev[sizeof(prev) - 1] = 0;
    FsFile dir, entry;
    if (!dir.open("/samples")) return false;
    char name[64];
    out[0] = 0;
    while (entry.openNext(&dir, O_RDONLY)) {
        if (entry.isDir()) {
            entry.getName(name, sizeof(name));
            if (name[0] != '.' && (!prev[0] || strcasecmp(name, prev) > 0) &&
                (!out[0] || strcasecmp(name, out) < 0)) {
                strncpy(out, name, outLen - 1);
                out[outLen - 1] = 0;
            }
        }
        entry.close();
    }
    return out[0] != 0;
}

bool fileExists(const char* path) { return mounted && card.exists(path); }

bool wavInfo(const char* path, WavInfo& info) {
    if (!mounted) return false;
    FsFile f;
    if (!f.open(path, O_RDONLY)) return false;
    const bool ok = readWavInfo(f, info) == WavError::Ok;
    f.close();
    return ok;
}

bool packExists(const char* name) {
    if (!mounted || !name || !name[0]) return false;
    char path[128];
    snprintf(path, sizeof(path), "/samples/%s", name);
    return card.exists(path);
}

bool choosePack(const char* preferred, char* out, size_t outLen) {
    char path[128];
    if (preferred && preferred[0]) {
        snprintf(path, sizeof(path), "/samples/%s", preferred);
        if (card.exists(path)) {
            strncpy(out, preferred, outLen - 1);
            out[outLen - 1] = 0;
            return true;
        }
        Console.printf("[sd] pack '%s' not found, using first pack\n", preferred);
    }
    return nextPackName("", out, outLen);
}

int packIndex(const char* name) {
    char cur[64] = "";
    for (int i = 0; nextPackName(cur, cur, sizeof(cur)); ++i)
        if (strcasecmp(cur, name) == 0) return i;
    return -1;
}

bool packByIndex(int index, char* out, size_t outLen, int& count) {
    char cur[64] = "";
    count = 0;
    while (nextPackName(cur, cur, sizeof(cur))) ++count;
    if (count == 0) return false;
    index %= count;
    if (index < 0) index += count;
    cur[0] = 0;
    for (int i = 0; i <= index; ++i) nextPackName(cur, cur, sizeof(cur));
    strncpy(out, cur, outLen - 1);
    out[outLen - 1] = 0;
    return true;
}

bool reserveFile(const char* path, uint32_t bytes, uint32_t& firstSector, uint32_t& sectors) {
    if (!mounted) return false;
    FsFile f;
    if (card.exists(path)) {
        if (!f.open(path, O_RDONLY)) return false;
    } else if (!f.open(path, O_RDWR | O_CREAT) || !f.preAllocate(bytes)) {
        f.close();
        card.remove(path);
        return false;
    }
    Sector_t first = 0, last = 0;
    const bool contiguous = f.contiguousRange(&first, &last);
    f.close();
    if (!contiguous || (last - first + 1) * 512 < bytes) {
        Console.printf("[sd] %s is not a contiguous %lu bytes; delete it to recreate\n", path,
                      static_cast<unsigned long>(bytes));
        return false;
    }
    firstSector = static_cast<uint32_t>(first);
    sectors = bytes / 512;
    return true;
}

bool readSector(uint32_t sector, uint8_t* dst) { return mounted && card.card()->readSector(sector, dst); }

// With DEDICATED_SPI, SdFat leaves a multi-block write open after each
// sector, ready for the next. The power log's sectors are written one at a
// time while the board may reset at any moment (power returning, or dying),
// so close it every time: a card reset mid-write ignores CMD0 until its
// power is cut.
bool writeSector(uint32_t sector, const uint8_t* src) {
    return mounted && card.card()->writeSector(sector, src) && card.card()->syncDevice();
}

void finishTransfers() {
    if (mounted) card.card()->syncDevice();
}

uint32_t cardSectors() { return mounted ? static_cast<uint32_t>(card.card()->sectorCount()) : 0; }

bool readSectors(uint32_t sector, uint8_t* dst, size_t count) {
    return mounted && card.card()->readSectors(sector, dst, count);
}

bool writeSectors(uint32_t sector, const uint8_t* src, size_t count) {
    return mounted && card.card()->writeSectors(sector, src, count);
}

void listDir(const char* path) {
    FsFile dir, entry;
    if (!mounted || !dir.open(path)) {
        Console.printf("[sd] cannot open %s\n", path);
        return;
    }
    Console.printf("[sd] %s:\n", path);
    char name[96];
    while (entry.openNext(&dir, O_RDONLY)) {
        entry.getName(name, sizeof(name));
        if (entry.isDir()) Console.printf("  %s/\n", name);
        else Console.printf("  %s  (%lu bytes)\n", name, static_cast<unsigned long>(entry.fileSize()));
        entry.close();
    }
}

bool readTextFile(const char* path, char* buf, size_t bufLen, size_t& outLen) {
    outLen = 0;
    FsFile f;
    if (!f.open(path, O_RDONLY)) return false;
    const int n = f.read(buf, bufLen - 1);
    if (n < 0) return false;
    buf[n] = 0;
    outLen = static_cast<size_t>(n);
    return true;
}

} // namespace sd
