// The USB companion app (companion/): its '$' sentences arrive on the USB
// serial console (host_protocol.h). Disk activity ($HDA) is kept here, like
// the HDDSynthPro link's (hdd_link.h); commands go back to main.cpp, which
// owns the state they change. Core 0 only.
#pragma once

#include <stddef.h>

#include "host_protocol.h"
#include "link_parser.h"

namespace usbhost {

// A complete '$' line from the console (without its line ending). True if
// it was a command for the caller to run.
bool handleLine(const char* line, size_t len, HostCommand& cmd);

bool present();     // an $HDA sentence within the last second
LinkState state();  // the latest $HDA (meaningful while present())
bool appActive();   // any valid sentence within the last 3 s (worth sending status to)

// Send a sentence to the app: `body` is everything between '$' and '*'.
// Written straight to Serial, not through Console, so it stays out of the
// OLED's debug screen.
void send(const char* body);
void printStats();  // one line for the 's' output, once the app has spoken

} // namespace usbhost
