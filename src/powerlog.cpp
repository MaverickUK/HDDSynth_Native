#include "powerlog.h"

#include <Arduino.h>
#include <stdarg.h>

#include "powerlog_format.h"
#include "sd_streamer.h"

namespace powerlog {

namespace {

constexpr char PATH[] = "/powerlog.bin";
constexpr uint32_t LOG_BYTES = 64 * 1024;  // 128 sectors: ~2 min at 10 lines/s, ~10 min at 2 lines/s
constexpr uint32_t FLUSH_MS = 250;

bool ready = false;    // file reserved, sectors known
bool logging = false;  // a session is running
bool dirty = false;    // the current sector has text not yet on the card
bool full = false;     // out of sectors: further lines go to Serial only
bool serialOn = true;  // false after a real power loss (see start())
uint32_t firstSector = 0;
uint32_t sectorCount = 0;
uint32_t lastSession = 0;  // session found on the card at boot (0 = none)
uint32_t session = 0;
uint16_t sectorIndex = 0;
char text[TEXT_BYTES];
size_t textLen = 0;
uint32_t lastFlushMs = 0;
uint8_t sectorBuf[SECTOR_BYTES];

void writeCurrent() {
    encodeSector(sectorBuf, session, sectorIndex, text, textLen);
    sd::writeSector(firstSector + sectorIndex, sectorBuf);  // a failed write just loses this sector
    dirty = false;
    lastFlushMs = millis();
}

// Walk the session that starts at sector 0, calling onText for each sector's
// text. Stops at the first sector that is blank, torn, out of order or from
// an older session. Returns the session number (0 if there is none).
template <typename F>
uint32_t readSession(F onText) {
    uint32_t found = 0;
    for (uint32_t i = 0; i < sectorCount; ++i) {
        if (!sd::readSector(firstSector + i, sectorBuf)) break;
        uint32_t s;
        uint16_t index;
        const char* t;
        const int n = decodeSector(sectorBuf, s, index, t);
        if (n < 0 || index != i || (i > 0 && s != found)) break;
        found = s;
        onText(t, static_cast<size_t>(n));
    }
    return found;
}

void append(const char* line, size_t len) {
    if (!logging || full) return;
    if (textLen + len > TEXT_BYTES) {
        writeCurrent();
        if (sectorIndex + 1u >= sectorCount) {
            full = true;
            return;
        }
        ++sectorIndex;
        textLen = 0;
    }
    memcpy(text + textLen, line, len);
    textLen += len;
    dirty = true;
}

} // namespace

bool begin() {
    ready = sd::reserveFile(PATH, LOG_BYTES, firstSector, sectorCount);
    if (!ready) {
        Serial.println("[powerlog] cannot reserve /powerlog.bin, power-loss logging is off");
        return false;
    }
    size_t bytes = 0;
    lastSession = readSession([&](const char*, size_t n) { bytes += n; });
    if (lastSession) {
        Serial.printf("[powerlog] last power loss logged (session %lu, %u bytes): send 'l' to print it\n",
                      static_cast<unsigned long>(lastSession), static_cast<unsigned>(bytes));
    } else {
        Serial.println("[powerlog] no power loss logged yet");
    }
    return true;
}

void start(const char* reason, bool serialToo) {
    serialOn = serialToo;
    if (!ready) return;
    session = lastSession + 1;
    lastSession = session;
    sectorIndex = 0;
    textLen = 0;
    full = false;
    logging = true;
    logf("[powerlog] session %lu: %s, uptime %lu ms", static_cast<unsigned long>(session), reason,
         static_cast<unsigned long>(millis()));
    writeCurrent();  // mark the new session on the card straight away
}

void logf(const char* fmt, ...) {
    char line[160];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line, sizeof(line) - 1, fmt, args);
    va_end(args);
    if (n < 0) return;
    if (n > static_cast<int>(sizeof(line)) - 2) n = sizeof(line) - 2;
    line[n++] = '\n';
    line[n] = 0;
    if (serialOn) Serial.print(line);
    append(line, static_cast<size_t>(n));
}

void service() {
    if (logging && dirty && millis() - lastFlushMs >= FLUSH_MS) writeCurrent();
}

void flush() {
    if (logging && dirty) writeCurrent();
}

void printLast() {
    if (!ready) {
        Serial.println("[powerlog] not available");
        return;
    }
    flush();
    Serial.println("[powerlog] ---- last power loss ----");
    const uint32_t s = readSession([](const char* t, size_t n) { Serial.write(t, n); });
    Serial.println(s ? "[powerlog] ---- end ----" : "[powerlog] nothing logged");
}

} // namespace powerlog
