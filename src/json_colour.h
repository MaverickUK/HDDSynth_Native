// Parse a colour from JSON: "#RRGGBB" / "RRGGBB" or [r, g, b] (0-255 each).
// Shared by settings.json and pack.json. Returns false (leaving `out`
// untouched) if the value isn't a valid colour.
#pragma once

#include <ArduinoJson.h>
#include <stdint.h>

bool parseColour(JsonVariantConst v, uint8_t out[3]);
