// The activity link from HDDSynthPro (EXPERIMENTAL_HDDSYNTHPRO_SUPPORT): a
// one-way UART into J6 pin 3 (GP25, UART1 RX), so it shares J6 with the
// OLED. Core 0 only; nothing here blocks. The sentences are parsed by
// link_parser.h.
#pragma once

#include "link_parser.h"

namespace hddlink {

void begin();       // start UART1 on GP25 (call once, only with the setting on)
void service();     // read whatever has arrived; call every loop() pass
bool enabled();     // begin() has been called
bool present();     // a valid sentence within the last second
LinkState state();  // the latest $HDA (meaningful while present())
const HeadFeed& heads();  // the latest $HDE, and whether they're still arriving
// Send a sentence back to HDDSynthPro on J6 pin 4 (GP24, UART1 TX), the
// link's other direction: `body` is everything between '$' and '*'.
void send(const char* body);
void printStats();  // one line for the 's' output

} // namespace hddlink
