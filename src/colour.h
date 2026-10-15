// Colour helpers. Pure C++ (no Arduino), host-tested.
#pragma once

#include <stdint.h>

// A fully saturated, full-brightness colour from a hue in degrees (0-359,
// wrapped): never grey, white or dim, so it always shows clearly on the LED.
inline void hueToRgb(uint16_t hue, uint8_t& r, uint8_t& g, uint8_t& b) {
    hue %= 360;
    const uint8_t rise = static_cast<uint8_t>((hue % 60) * 255 / 60);
    const uint8_t fall = static_cast<uint8_t>(255 - rise);
    switch (hue / 60) {
        case 0: r = 255; g = rise; b = 0; break;    // red -> yellow
        case 1: r = fall; g = 255; b = 0; break;    // yellow -> green
        case 2: r = 0; g = 255; b = rise; break;    // green -> cyan
        case 3: r = 0; g = fall; b = 255; break;    // cyan -> blue
        case 4: r = rise; g = 0; b = 255; break;    // blue -> magenta
        default: r = 255; g = 0; b = fall; break;   // magenta -> red
    }
}
