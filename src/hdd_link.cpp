#include "hdd_link.h"

#include <Arduino.h>
#include <hardware/gpio.h>

#include "config.h"
#include "console.h"

namespace hddlink {

namespace {

bool started = false;
bool wasPresent = false;
LinkParser parser;
LinkWatch watch;
HeadFeed headFeed;
uint32_t sent = 0;

char className(ActivityClass c) {
    switch (c) {
        case ActivityClass::Idle: return 'I';
        case ActivityClass::Sequential: return 'S';
        case ActivityClass::Random: return 'R';
    }
    return '?';
}

} // namespace

void begin() {
    // UART1 is Arduino-Pico's Serial2. The hardware FIFO holds 32 bytes; a
    // bigger software one (filled by the UART interrupt, on core 0) covers a
    // slow loop pass such as an SD refill.
    Serial2.setRX(cfg::PIN_LINK_RX);
    Serial2.setTX(cfg::PIN_LINK_TX);
    Serial2.setFIFOSize(512);
    // The receive buffer above; transmit (the $HDM replies) uses the 32-byte
    // hardware FIFO, and send() never waits for room.
    Serial2.begin(cfg::LINK_BAUD);
    // With nothing attached, keep the input from floating into noise (a
    // UART line idles high).
    gpio_pull_up(cfg::PIN_LINK_RX);
    started = true;
    Console.println("[link] HDDSynthPro activity link listening on J6 (GP25, 115200 baud)");
}

void service() {
    if (!started) return;
    const uint32_t now = millis();
    for (int n = Serial2.available(); n > 0; --n) {
        switch (parser.feed(static_cast<char>(Serial2.read()))) {
            case LinkParser::Got::Activity: watch.seen(now); break;
            case LinkParser::Got::Head:
                watch.seen(now);
                headFeed.got(parser.head(), now);
                break;
            case LinkParser::Got::Nothing: break;
        }
    }
    const bool p = watch.present(now);
    if (p != wasPresent) {
        wasPresent = p;
        Console.println(p ? "[link] up" : "[link] lost, using the activity input");
    }
}

bool enabled() { return started; }
bool present() { return started && watch.present(millis()); }
LinkState state() { return parser.state(); }
const HeadFeed& heads() { return headFeed; }

void send(const char* body) {
    if (!started) return;
    char buf[96];
    const size_t n = formatSentence(buf, sizeof(buf), body);
    // Never wait: a sentence that doesn't fit the transmit FIFO now is
    // skipped (the model is resent every 2 s anyway).
    if (n && Serial2.availableForWrite() >= static_cast<int>(n)) {
        Serial2.write(buf, n);
        ++sent;
    }
}

void printStats() {
    if (!started) return;
    const LinkState s = parser.state();
    Console.printf("[stats] link: %s busy=%d class=%c valid=%lu rejected=%lu heads=%lu%s sent=%lu\n",
                   present() ? "up" : "lost", s.busy, className(s.cls), static_cast<unsigned long>(parser.valid()),
                   static_cast<unsigned long>(parser.rejected()), static_cast<unsigned long>(headFeed.seq),
                   headFeed.watch.present(millis()) ? " (arriving)" : "", static_cast<unsigned long>(sent));
}

} // namespace hddlink
