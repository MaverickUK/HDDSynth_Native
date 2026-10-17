#include "oled_screens.h"

#include <stdio.h>
#include <string.h>

namespace oled {

namespace {

constexpr int BODY_BOTTOM = Canvas::HEIGHT - 1;            // last row
constexpr int BODY_H = Canvas::HEIGHT - BODY_TOP;          // 52 rows
constexpr int MAX_TITLE_CHARS = Canvas::WIDTH / Canvas::CHAR_W;

int clampPercent(int p) { return p < 0 ? 0 : (p > 100 ? 100 : p); }

} // namespace

void drawTitle(Canvas& c, const char* title) {
    char t[MAX_TITLE_CHARS + 1];
    strncpy(t, title, MAX_TITLE_CHARS);
    t[MAX_TITLE_CHARS] = 0;
    c.fillRect(0, 0, Canvas::WIDTH, TITLE_H);
    const int x = (Canvas::WIDTH - Canvas::textWidth(t)) / 2;
    c.text(x < 0 ? 0 : x, 1, t, /*on*/ false);
}

void drawGraph(Canvas& c, const char* title, const SampleHistory& h) {
    c.clear();
    drawTitle(c, title);
    const int n = h.count() < Canvas::WIDTH ? h.count() : Canvas::WIDTH;
    for (int ago = 0; ago < n; ++ago) {
        const int height = (h.ago(ago) * BODY_H + 127) / 255;  // rounded
        if (height > 0) c.vLine(Canvas::WIDTH - 1 - ago, BODY_BOTTOM - height + 1, height);
    }
}

void drawVolume(Canvas& c, int percent) {
    percent = clampPercent(percent);
    c.clear();
    char title[24];
    snprintf(title, sizeof(title), "Volume %d%%", percent);
    drawTitle(c, title);

    constexpr int STEPS = 20;  // 5%, 10%, ... 100%
    constexpr int PITCH = 6;   // pixels per column
    constexpr int THICK = 4;
    constexpr int LEFT = (Canvas::WIDTH - ((STEPS - 1) * PITCH + THICK)) / 2;
    constexpr int MIN_H = 3;
    for (int i = 0; i < STEPS; ++i) {
        const int level = (i + 1) * 5;
        const int height = MIN_H + i * (BODY_H - MIN_H) / (STEPS - 1);
        const int x = LEFT + i * PITCH;
        const int top = BODY_BOTTOM - height + 1;
        if (level <= percent) c.fillRect(x, top, THICK, height);
        else c.vLine(x + THICK / 2 - 1, top, height);  // thin, centred in the column's slot
    }
}

void drawBalance(Canvas& c, int accessPercent) {
    accessPercent = clampPercent(accessPercent);
    c.clear();
    drawTitle(c, "Balance");

    constexpr int TRACK_L = 6, TRACK_R = Canvas::WIDTH - 7, TRACK_Y = 30;
    constexpr int CENTRE = (TRACK_L + TRACK_R) / 2;
    c.hLine(TRACK_L, TRACK_Y, TRACK_R - TRACK_L + 1);
    c.vLine(TRACK_L, TRACK_Y - 4, 9);  // ends
    c.vLine(TRACK_R, TRACK_Y - 4, 9);
    for (int y = TRACK_Y - 8; y <= TRACK_Y + 8; y += 2) c.pixel(CENTRE, y);  // dotted centre (balanced)

    // Marker: a solid block on the track, with a pointer above it.
    const int x = TRACK_L + (TRACK_R - TRACK_L) * accessPercent / 100;
    c.fillRect(x - 2, TRACK_Y - 6, 5, 13);
    for (int i = 0; i < 4; ++i) c.hLine(x - i, TRACK_Y - 11 + i, 2 * i + 1);  // small triangle

    char left[16], right[16];
    snprintf(left, sizeof(left), "Idle %d%%", 100 - accessPercent);
    snprintf(right, sizeof(right), "%d%% Access", accessPercent);
    const int y = Canvas::HEIGHT - Canvas::CHAR_H;
    c.text(0, y, left);
    c.text(Canvas::WIDTH - Canvas::textWidth(right), y, right);
}

void drawConsole(Canvas& c, const TextConsole& t) {
    c.clear();
    for (int i = 0; i < TextConsole::ROWS; ++i) c.text(0, i * Canvas::CHAR_H, t.row(i));
}

} // namespace oled
