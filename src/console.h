// Console output. Everything the firmware prints goes through `Console`:
// to USB serial, and into a small text buffer that the OLED's debug screen
// shows. Core 0 only (core 1 and interrupts never print).
//
// Use Console.print/printf/println exactly like Serial's. Input (reading
// serial commands) still uses Serial directly.
#pragma once

#include <Arduino.h>

#include "oled_model.h"

class ConsoleOut : public Print {
public:
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* buf, size_t n) override;
    using Print::write;

    // The last lines printed, for the OLED debug screen.
    const oled::TextConsole& text() const { return text_; }

private:
    oled::TextConsole text_;
};

extern ConsoleOut Console;
