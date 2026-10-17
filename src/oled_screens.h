// Drawing each OLED screen onto a Canvas. Pure C++ (no Arduino); the host
// tests render these to images.
//
// Layout shared by the graph and level screens: a 10-pixel lit title bar
// across the top with dark text, and the content below it.
#pragma once

#include "canvas.h"
#include "oled_model.h"

namespace oled {

constexpr int TITLE_H = 10;
constexpr int BODY_TOP = TITLE_H + 2;  // a 2-pixel gap under the title bar

void drawTitle(Canvas& c, const char* title);

// Title bar, then one column per sample (height = value / 255), newest on
// the right. Used for both the activity and the supercap screens.
void drawGraph(Canvas& c, const char* title, const SampleHistory& h);

// "Volume NN%": 20 rising columns (5% steps). Those up to the current level
// are thick, the rest thin.
void drawVolume(Canvas& c, int percent);

// "Balance": a track from idle (left) to access (right) with a centre mark;
// a marker shows the current balance, with both percentages underneath.
void drawBalance(Canvas& c, int accessPercent);

// The debug console: the last 8 lines of output, full screen.
void drawConsole(Canvas& c, const TextConsole& t);

} // namespace oled
