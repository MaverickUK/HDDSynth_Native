// The activity link from HDDSynthPro (EXPERIMENTAL_HDDSYNTHPRO_SUPPORT):
// NMEA-style ASCII lines over a one-way UART,
//
//   $HDA,1,<busy>,<class>,<op>*<checksum>\r\n
//
// busy 0/1, class I/S/R (idle, sequential, random), op R/W/- (read, write,
// none; parsed but not used yet), checksum two hex digits: the XOR of every
// byte between '$' and '*'. Sent on every change and every 200 ms. The
// protocol is described in HDDSynthPro's docs/activity-link.md.
// Platform independent (unit tested natively).
#pragma once

#include <stddef.h>
#include <stdint.h>

enum class ActivityClass : uint8_t { Idle, Sequential, Random };
enum class LinkOp : uint8_t { None, Read, Write };

struct LinkState {
    bool busy = false;
    ActivityClass cls = ActivityClass::Idle;
    LinkOp op = LinkOp::None;
};

// Parse one line, without its line ending. False if it isn't a valid
// version 1 $HDA sentence (fields after <op> are ignored: later versions
// add them at the end). Hex in the checksum may be upper or lower case.
bool parseLinkSentence(const char* line, size_t len, LinkState& out);

// Assembles lines from a byte stream and parses them. A '$' always starts a
// new line (as in NMEA), so noise before it is dropped; a line ends at '\n'
// (an optional '\r' before it is stripped).
class LinkParser {
public:
    static constexpr size_t MAX_LINE = 80;  // longer lines are dropped (without the line ending)

    // Feed one byte. True when it completes a valid sentence (see state()).
    bool feed(char c);
    const LinkState& state() const { return state_; }
    uint32_t valid() const { return valid_; }
    uint32_t rejected() const { return rejected_; }  // complete lines that failed (noise without a '$' isn't counted)

private:
    char buf_[MAX_LINE + 1];  // room for a '\r'
    size_t len_ = 0;
    bool inLine_ = false;   // seen a '$'
    bool tooLong_ = false;
    LinkState state_;
    uint32_t valid_ = 0;
    uint32_t rejected_ = 0;
};

// Whether the link is present: from a valid sentence until TIMEOUT_MS
// without one.
class LinkWatch {
public:
    static constexpr uint32_t TIMEOUT_MS = 1000;

    void seen(uint32_t nowMs) {
        lastMs_ = nowMs;
        ever_ = true;
    }
    bool present(uint32_t nowMs) const { return ever_ && nowMs - lastMs_ < TIMEOUT_MS; }

private:
    uint32_t lastMs_ = 0;
    bool ever_ = false;
};

// Which looping sample is at the access level while the drive is active:
// sequential.wav during sequential transfers (if the pack has one), else
// access.wav. The class the link reports can be Idle for a moment at a
// burst's edges, so the last busy class sticks until another replaces it.
enum class AccessSample : uint8_t { Access, Sequential };

class AccessChooser {
public:
    // `cls` is the latest class (Random when there is no link).
    AccessSample choose(ActivityClass cls, bool packHasSequential) {
        if (cls != ActivityClass::Idle) last_ = cls;
        return packHasSequential && last_ == ActivityClass::Sequential ? AccessSample::Sequential
                                                                        : AccessSample::Access;
    }
    // The sticky class itself (for the sequential LED colour).
    bool sequential() const { return last_ == ActivityClass::Sequential; }

private:
    ActivityClass last_ = ActivityClass::Random;
};
