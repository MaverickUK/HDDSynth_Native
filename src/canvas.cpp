#include "canvas.h"

#include <string.h>

#include "font5x7.h"

void Canvas::clear() { memset(buf_, 0, sizeof(buf_)); }

void Canvas::pixel(int x, int y, bool on) {
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
    uint8_t& b = buf_[(y / 8) * WIDTH + x];
    const uint8_t bit = static_cast<uint8_t>(1u << (y % 8));
    if (on) b |= bit;
    else b &= static_cast<uint8_t>(~bit);
}

bool Canvas::get(int x, int y) const {
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return false;
    return buf_[(y / 8) * WIDTH + x] & (1u << (y % 8));
}

void Canvas::fillRect(int x, int y, int w, int h, bool on) {
    for (int yy = y; yy < y + h; ++yy)
        for (int xx = x; xx < x + w; ++xx) pixel(xx, yy, on);
}

int Canvas::text(int x, int y, const char* s, bool on) {
    for (; *s; ++s) {
        char c = *s;
        if (c < FONT5X7_FIRST || c > FONT5X7_LAST) c = '?';
        const uint8_t* glyph = &FONT5X7[(c - FONT5X7_FIRST) * 5];
        for (int col = 0; col < 5; ++col)
            for (int row = 0; row < 7; ++row)
                if (glyph[col] & (1u << row)) pixel(x + col, y + row, on);
        x += CHAR_W;
    }
    return x;
}

int Canvas::textWidth(const char* s) {
    const int n = static_cast<int>(strlen(s));
    return n ? n * CHAR_W - 1 : 0;  // no trailing gap
}
