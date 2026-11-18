#include "hdd_link.h"

#include <Arduino.h>
#include <hardware/gpio.h>

#include "config.h"
#include "console.h"
#include "tx_queue.h"

namespace hddlink {

namespace {

bool started = false;
bool wasPresent = false;
LinkParser parser;
LinkWatch watch;
HeadFeed headFeed;
uint32_t sent = 0;
uint32_t skipped = 0;   // sentences that didn't fit the queue
uint32_t commands = 0;  // $HDQ/$HDC received

// Sentences waiting for the UART's 32-byte transmit FIFO: a pack list is a
// few KB. On the heap, made by begin(), so it costs nothing with the link off.
constexpr size_t TX_QUEUE_BYTES = 2048;
TxQueue* txQueue = nullptr;

// Commands received, for main.cpp's loop to run. SynthMinder sends one at a
// time; a few cover anything else.
constexpr size_t COMMANDS_MAX = 4;
HostCommand commandQueue[COMMANDS_MAX];
size_t commandFirst = 0, commandCount = 0;

// Move what's queued into the UART's FIFO, as far as it has room.
void drain() {
    if (!txQueue) return;
    while (!txQueue->empty() && Serial2.availableForWrite() > 0) Serial2.write(txQueue->pop());
}

bool queueSentence(const char* body, uint32_t waitMs) {
    if (!started) return false;
    char buf[SENTENCE_MAX_LINE + 4];
    const size_t n = formatSentence(buf, sizeof(buf), body);
    if (!n) return false;
    const uint32_t since = millis();
    while (!txQueue->push(buf, n)) {
        if (millis() - since >= waitMs) {
            ++skipped;
            return false;
        }
        drain();
    }
    ++sent;
    drain();
    return true;
}

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
    // The receive buffer above. Transmit has only the 32-byte hardware FIFO,
    // so what goes out waits in txQueue and service() feeds it in.
    static char* txBytes = new char[TX_QUEUE_BYTES];
    static TxQueue queue(txBytes, TX_QUEUE_BYTES);
    txQueue = &queue;
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
            case LinkParser::Got::Command:
                watch.seen(now);
                ++commands;
                if (commandCount < COMMANDS_MAX) {
                    commandQueue[(commandFirst + commandCount) % COMMANDS_MAX] = parser.command();
                    ++commandCount;
                }
                break;
            case LinkParser::Got::Nothing: break;
        }
    }
    drain();
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

bool takeCommand(HostCommand& cmd) {
    if (!commandCount) return false;
    cmd = commandQueue[commandFirst];
    commandFirst = (commandFirst + 1) % COMMANDS_MAX;
    --commandCount;
    return true;
}

// The model is resent every 2 s anyway, so it never waits.
void send(const char* body) { queueSentence(body, 0); }

// 50 ms covers a pack list: the queue holds about 25 lines, and the UART
// sends one in about 7 ms.
void reply(const char* body) { queueSentence(body, 50); }

void printStats() {
    if (!started) return;
    const LinkState s = parser.state();
    Console.printf("[stats] link: %s busy=%d class=%c valid=%lu rejected=%lu heads=%lu%s commands=%lu sent=%lu "
                   "skipped=%lu\n",
                   present() ? "up" : "lost", s.busy, className(s.cls), static_cast<unsigned long>(parser.valid()),
                   static_cast<unsigned long>(parser.rejected()), static_cast<unsigned long>(headFeed.seq),
                   headFeed.watch.present(millis()) ? " (arriving)" : "", static_cast<unsigned long>(commands),
                   static_cast<unsigned long>(sent), static_cast<unsigned long>(skipped));
}

} // namespace hddlink
