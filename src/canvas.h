// A 128x64 one-bit drawing surface for the OLED. Pure C++ (no Arduino),
// host-tested, and the host tests render it to images for previews.
//
// The buffer uses the SSD1306's own layout, so the driver sends it as-is:
// 8 "pages" of 8 pixel rows; each byte is one column of a page, bit 0 at
// the top.
#pragma once

#include <stddef.h>
#include <stdint.h>

class Canvas {
public:
    static constexpr int WIDTH = 128;
    static constexpr int HEIGHT = 64;
    static constexpr size_t BYTES = WIDTH * HEIGHT / 8;
    static constexpr int CHAR_W = 6;  // 5 pixel glyph + 1 pixel gap
    static constexpr int CHAR_H = 8;

    void clear();
    void pixel(int x, int y, bool on = true);
    bool get(int x, int y) const;
    // Filled rectangle; anything off the edges is clipped.
    void fillRect(int x, int y, int w, int h, bool on = true);
    void hLine(int x, int y, int w, bool on = true) { fillRect(x, y, w, 1, on); }
    void vLine(int x, int y, int h, bool on = true) { fillRect(x, y, 1, h, on); }

    // Draw ASCII text with the 5x7 font, top-left at (x, y). `on` false
    // draws dark text (for use on a lit background). Returns the x after it.
    int text(int x, int y, const char* s, bool on = true);
    static int textWidth(const char* s);

    const uint8_t* data() const { return buf_; }

private:
    uint8_t buf_[BYTES] = {0};
};
