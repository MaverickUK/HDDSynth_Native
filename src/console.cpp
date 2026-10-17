#include "console.h"

ConsoleOut Console;

size_t ConsoleOut::write(uint8_t c) {
    text_.write(static_cast<char>(c));
    return Serial.write(c);
}

size_t ConsoleOut::write(const uint8_t* buf, size_t n) {
    for (size_t i = 0; i < n; ++i) text_.write(static_cast<char>(buf[i]));
    return Serial.write(buf, n);
}
