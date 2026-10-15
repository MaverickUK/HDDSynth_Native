// Power-loss log. Core 0 only (it writes to the SD card).
//
// When power fails, everything we want to know (the supercap curve during
// spindown, and how long the board survives afterwards) happens while USB
// may be gone and just before the chip loses power and RAM. So from the
// moment power loss is detected, power-related lines are also written to
// /powerlog.bin on the SD card, flushed every FLUSH_MS, and the last session
// can be printed after the next boot with the serial 'l' command.
//
// Only the latest session is kept: each power loss overwrites the previous one.
#pragma once

namespace powerlog {

// At boot, after the card is mounted: reserve /powerlog.bin and summarise
// the previous session. Logging simply stays off if this fails.
bool begin();

// Power loss detected: start a new session (overwrites the previous one).
void start(const char* reason);

// Print a line to Serial and, while a session is running, append it to the
// log. A newline is added.
void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Call from loop(): writes the current sector if it changed and FLUSH_MS has
// passed since the last write.
void service();

// Write the current sector now (e.g. just before rebooting).
void flush();

// Serial 'l': print the last session.
void printLast();

} // namespace powerlog
