#include "sd_streamer.h"

#include <Arduino.h>
#include <SPI.h>
#include <SdFat.h>
#include <hardware/gpio.h>

#include "config.h"

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
        Serial.printf("[sd] mount failed: error 0x%02X data 0x%02X\n",
                      card.sdErrorCode(), card.sdErrorData());
    }
    return mounted;
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
            Serial.printf("[sd] reopen failed: %s\n", s->path);
            return false;
        }
    }
    return true;
}

// Quick check for a card: send CMD0 (GO_IDLE_STATE) and look for the 0x01
// reply. Takes well under 1 ms with or without a card. card.begin() instead
// waits up to 2 s for an absent card, longer than the watchdog allows, and
// power-loss detection would be blind for that time.
bool cardResponds() {
    setSpiPins();
    pinMode(cfg::PIN_SD_CS, OUTPUT);
    digitalWrite(cfg::PIN_SD_CS, HIGH);
    SPI1.begin();
    SPI1.beginTransaction(SPISettings(cfg::SD_PROBE_HZ, MSBFIRST, SPI_MODE0));
    for (int i = 0; i < 10; ++i) SPI1.transfer(0xFF);  // 80 clocks with CS high: wake up
    digitalWrite(cfg::PIN_SD_CS, LOW);
    const uint8_t cmd0[] = {0x40, 0x00, 0x00, 0x00, 0x00, 0x95};
    for (uint8_t b : cmd0) SPI1.transfer(b);
    uint8_t r1 = 0xFF;
    for (int i = 0; i < 16 && r1 == 0xFF; ++i) r1 = SPI1.transfer(0xFF);
    digitalWrite(cfg::PIN_SD_CS, HIGH);
    SPI1.transfer(0xFF);
    SPI1.endTransaction();
    SPI1.end();
    return r1 == 0x01;
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
    Serial.println("[sd] card down, playing from buffers");
}

void tryRemount() {
    const uint32_t now = millis();
    if (static_cast<int32_t>(now - nextRemountMs) < 0) return;
    nextRemountMs = now + cfg::SD_REMOUNT_INTERVAL_MS;
    if (!cardResponds()) return;  // still absent: try again later
    if (mount() && reopenAll()) {
        st.recoveries++;
        Serial.printf("[sd] card back after %lu ms\n", static_cast<unsigned long>(millis() - downSinceMs));
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
        Serial.printf("[sd] read error on %s at %lu (got %d), attempt %d\n", s.path,
                      static_cast<unsigned long>(s.pos), n, attempt + 1);
    }
    Serial.printf("[sd] giving up on reads after %lu ms\n", static_cast<unsigned long>(millis() - firstTry));
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

bool begin() { return mount(); }

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
        Serial.printf("[sd] cannot open %s\n", path);
        return false;
    }
    uint8_t header[1024];
    const int n = f.read(header, sizeof(header));
    const WavError e = parseWavHeader(header, n > 0 ? n : 0, static_cast<uint32_t>(f.fileSize()), s.info);
    if (e != WavError::Ok || !wavIsPlayable(s.info, cfg::SAMPLE_RATE)) {
        Serial.printf("[sd] %s: %s (fmt %u, %u ch, %lu Hz, %u bit)\n", path,
                      e == WavError::Ok ? "wrong format" : wavErrorName(e), s.info.format,
                      s.info.channels, static_cast<unsigned long>(s.info.sampleRate), s.info.bitsPerSample);
        f.close();
        return false;
    }
    s.open = true;
    return true;
}

bool prime(Stream& s) {
    const int slot = slotOf(s);
    if (!mounted || slot < 0 || !s.open || !audio::isIdle(s.voice)) return false;

    s.ring->reset();
    audio::voice(s.voice).eof.store(false, std::memory_order_relaxed);
    s.pos = 0;
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
    uint8_t header[1024];
    const int n = f.read(header, sizeof(header));
    WavInfo info;
    if (parseWavHeader(header, n > 0 ? n : 0, static_cast<uint32_t>(f.fileSize()), info) != WavError::Ok ||
        !wavIsPlayable(info, cfg::SAMPLE_RATE)) {
        return false;
    }
    if (rp2040.getFreeHeap() < static_cast<int>(info.dataBytes + cfg::HEAP_RESERVE_BYTES)) {
        Serial.printf("[sd] %s (%lu bytes) won't fit in RAM, free heap %d\n", path,
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

bool choosePack(const char* preferred, char* out, size_t outLen) {
    char path[128];
    if (preferred && preferred[0]) {
        snprintf(path, sizeof(path), "/samples/%s", preferred);
        if (card.exists(path)) {
            strncpy(out, preferred, outLen - 1);
            out[outLen - 1] = 0;
            return true;
        }
        Serial.printf("[sd] pack '%s' not found, using first pack\n", preferred);
    }
    FsFile dir, entry;
    if (!dir.open("/samples")) return false;
    char best[64] = {0};
    char name[64];
    while (entry.openNext(&dir, O_RDONLY)) {
        if (entry.isDir()) {
            entry.getName(name, sizeof(name));
            if (name[0] != '.' && (!best[0] || strcasecmp(name, best) < 0)) {
                strncpy(best, name, sizeof(best) - 1);
            }
        }
        entry.close();
    }
    if (!best[0]) return false;
    strncpy(out, best, outLen - 1);
    out[outLen - 1] = 0;
    return true;
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
