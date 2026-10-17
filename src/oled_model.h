// The OLED's data, separate from drawing and from the hardware. Pure C++
// (no Arduino), host-tested.
#pragma once

#include <stdint.h>
#include <string.h>

namespace oled {

// The last 128 samples (one per screen column), 0-255. Newest is drawn on the
// right; each new sample pushes the rest one column to the left.
class SampleHistory {
public:
    static constexpr int SIZE = 128;

    void push(uint8_t v) {
        values_[next_] = v;
        next_ = (next_ + 1) % SIZE;
        if (count_ < SIZE) ++count_;
    }
    int count() const { return count_; }
    // ago = 0 is the newest sample, 1 the one before, ...
    uint8_t ago(int ago) const { return values_[(next_ - 1 - ago + 2 * SIZE) % SIZE]; }

private:
    uint8_t values_[SIZE] = {0};
    int next_ = 0;
    int count_ = 0;
};

// Measures what share of each period a signal was on, e.g. HDD activity.
// Feed it every loop pass; it reports 0-255 once per period.
class DutyMeter {
public:
    explicit DutyMeter(uint32_t periodMs) : periodMs_(periodMs ? periodMs : 1) {}

    // `on` is the state since the previous call. Returns true when a period
    // completed, with its result in `out`.
    bool update(bool on, uint32_t nowMs, uint8_t& out) {
        if (!started_) {
            started_ = true;
            lastMs_ = periodStartMs_ = nowMs;
            return false;
        }
        const uint32_t dt = nowMs - lastMs_;
        lastMs_ = nowMs;
        if (on) onMs_ += dt;
        if (nowMs - periodStartMs_ < periodMs_) return false;
        const uint32_t span = nowMs - periodStartMs_;
        const uint32_t v = (onMs_ > span ? span : onMs_) * 255 / span;
        out = static_cast<uint8_t>(v);
        onMs_ = 0;
        periodStartMs_ = nowMs;
        return true;
    }

private:
    uint32_t periodMs_;
    bool started_ = false;
    uint32_t lastMs_ = 0, periodStartMs_ = 0, onMs_ = 0;
};

// The last few lines of console output, wrapped to the screen width, for the
// debug screen.
class TextConsole {
public:
    static constexpr int ROWS = 8;
    static constexpr int COLS = 21;  // 128 px / 6 px per character

    void write(char c) {
        if (c == '\r') return;
        if (c == '\n') {
            newLine();
            return;
        }
        if (col_ == COLS) newLine();  // wrap
        rows_[current_][col_++] = c;
        rows_[current_][col_] = 0;
        ++version_;
    }
    void write(const char* s) {
        while (*s) write(*s++);
    }
    // Row 0 is the oldest on screen, ROWS - 1 the newest (possibly unfinished).
    const char* row(int i) const { return rows_[(current_ + 1 + i) % ROWS]; }
    uint32_t version() const { return version_; }  // changes whenever the text does

private:
    void newLine() {
        current_ = (current_ + 1) % ROWS;
        rows_[current_][0] = 0;
        col_ = 0;
        ++version_;
    }

    char rows_[ROWS][COLS + 1] = {{0}};
    int current_ = ROWS - 1;
    int col_ = 0;
    uint32_t version_ = 0;
};

// Which temporary screen (if any) is showing over the selected mode: the
// volume or balance screen stays up until `holdMs` after the last change.
class Overlay {
public:
    enum class Kind : uint8_t { None, Volume, Balance };

    explicit Overlay(uint32_t holdMs) : holdMs_(holdMs) {}

    void show(Kind k, uint32_t nowMs) {
        kind_ = k;
        sinceMs_ = nowMs;
    }
    Kind current(uint32_t nowMs) {
        if (kind_ != Kind::None && nowMs - sinceMs_ >= holdMs_) kind_ = Kind::None;
        return kind_;
    }

private:
    uint32_t holdMs_;
    Kind kind_ = Kind::None;
    uint32_t sinceMs_ = 0;
};

} // namespace oled
