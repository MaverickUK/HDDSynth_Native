#include "hdd_link.h"

#include <Arduino.h>
#include <hardware/gpio.h>

#include "config.h"
#include "console.h"
#include "tx_drip.h"

namespace hddlink {

namespace {

bool started = false;
bool wasPresent = false;
LinkParser parser;
LinkWatch watch;
HeadFeed headFeed;
LinkWatch opticalWatch;
uint32_t opticalCount = 0;
uint32_t sent = 0;
TxDrip tx;  // the $HDM going out (send), fed to the FIFO by service()

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
    // The receive buffer above; transmit (the $HDM replies) goes through the
    // 32-byte hardware FIFO a byte at a time as it has room (TxDrip), so
    // nothing ever waits for it.
    Serial2.begin(cfg::LINK_BAUD);
    // With nothing attached, keep the input from floating into noise (a
    // UART line idles high).
    gpio_pull_up(cfg::PIN_LINK_RX);
    started = true;
    Console.println("[link] HDDSynthPro activity link listening on J6 (GP25, 115200 baud)");
}

void service() {
    if (!started) return;
    // The reply going out: availableForWrite() is 1 while a byte fits (it
    // never says how many), and a write then doesn't wait.
    tx.drain([] { return Serial2.availableForWrite() > 0; },
             [](char c) { Serial2.write(static_cast<uint8_t>(c)); });
    const uint32_t now = millis();
    for (int n = Serial2.available(); n > 0; --n) {
        switch (parser.feed(static_cast<char>(Serial2.read()))) {
            case LinkParser::Got::Activity: watch.seen(now); break;
            case LinkParser::Got::Head:
                watch.seen(now);
                headFeed.got(parser.head(), now);
                break;
            case LinkParser::Got::Optical:
                watch.seen(now);
                opticalWatch.seen(now);
                ++opticalCount;
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
bool opticalPresent() { return started && opticalWatch.present(millis()); }
OpticalState optical() { return opticalPresent() ? parser.optical() : OpticalState{}; }

void send(const char* body) {
    if (!started) return;
    char buf[96];
    const size_t n = formatSentence(buf, sizeof(buf), body);
    // Queued, then fed out by service() as the FIFO has room: never waits.
    // Waiting for availableForWrite() to cover the whole sentence never sent
    // anything (it says 1 or 0, v0.24.0). One still going out means this one
    // is skipped (the model is resent every 2 s anyway).
    if (tx.start(buf, n)) ++sent;
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
