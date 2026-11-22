// The activity link from HDDSynthPro (EXPERIMENTAL_HDDSYNTHPRO_SUPPORT):
// NMEA-style ASCII lines over a one-way UART,
//
//   $HDA,1,<busy>,<class>,<op>*<checksum>\r\n
//
// busy 0/1, class I/S/R (idle, sequential, random), op R/W/- (read, write,
// none; parsed but not used yet), checksum two hex digits: the XOR of every
// byte between '$' and '*'. Sent on every change and every 200 ms. The
// protocol is described in HDDSynthPro's docs/activity-link.md. The USB
// companion app sends the same sentence (host_protocol.h).
//
// Version 2 adds the head's target, for a modelled drive (drive.json):
//
//   $HDE,2,<pos>,<op>,<sectors>*<checksum>\r\n
//
// pos 0-65535: the command's LBA as a fraction of the disk (LBA x 65535 /
// total sectors); op R/W/-; sectors, the command's length. Sent at most
// every 2 ms, always the latest command (HDDSynthPro's
// docs/hddsynth-head-position-brief.md).
//
// Version 3 adds a CD-ROM drive on the same cable ($HDO, optical), sent only
// while the Bay has one, on every change and every 200 ms:
//
//   $HDO,3,<busy>,<class>,<motor>,<tray>*<checksum>\r\n
//
// busy and class as $HDA's but for the CD-ROM's reads; motor 1 while its
// disc spins (the Bay models when it spins up and down), tray O (open) or C
// (closed). The synth plays a CD-ROM pack (CDROM_SAMPLE_PACK) from it.
// Platform independent (unit tested natively).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sentence.h"

enum class ActivityClass : uint8_t { Idle, Sequential, Random };
enum class LinkOp : uint8_t { None, Read, Write };

struct LinkState {
    bool busy = false;
    ActivityClass cls = ActivityClass::Idle;
    LinkOp op = LinkOp::None;
};

// A command's target ($HDE).
struct HeadTarget {
    uint16_t pos = 0;      // LBA / total sectors, x 65535
    LinkOp op = LinkOp::None;
    uint32_t sectors = 0;
};

// The CD-ROM drive's state ($HDO).
struct OpticalState {
    bool busy = false;
    ActivityClass cls = ActivityClass::Idle;
    bool motor = false;     // the disc spins
    bool trayOpen = false;
    bool disk = true;       // a hard disk on the cable too; false: a CD-ROM drive alone (HDDSynthPro 0.36.0-cd.3)
    bool operator==(const OpticalState& o) const {
        return busy == o.busy && cls == o.cls && motor == o.motor && trayOpen == o.trayOpen && disk == o.disk;
    }
    bool operator!=(const OpticalState& o) const { return !(*this == o); }
};

// Parse an $HDO line (version 3 or later; later fields ignored). Its sixth
// field, `disk`, came later: absent (or anything but 0), a hard disk.
bool parseOpticalSentence(const char* line, size_t len, OpticalState& out);

// Parse an $HDE line (version 2 or later; later fields ignored).
bool parseHeadSentence(const char* line, size_t len, HeadTarget& out);

// Parse one line, without its line ending. False if it isn't a valid
// version 1 $HDA sentence (fields after <op> are ignored: later versions
// add them at the end). Hex in the checksum may be upper or lower case.
bool parseLinkSentence(const char* line, size_t len, LinkState& out);

// Assembles lines from a byte stream (see LineAssembler) and parses them.
class LinkParser {
public:
    static constexpr size_t MAX_LINE = SENTENCE_MAX_LINE;  // longer lines are dropped (without the line ending)

    enum class Got : uint8_t { Nothing, Activity, Head, Optical };
    // Feed one byte. Activity, Head or Optical when it completes a valid $HDA
    // (see state()), $HDE (see head()) or $HDO (see optical()).
    Got feed(char c);
    const LinkState& state() const { return state_; }
    const HeadTarget& head() const { return head_; }
    const OpticalState& optical() const { return optical_; }
    uint32_t valid() const { return valid_; }
    uint32_t rejected() const { return rejected_; }  // complete lines that failed (noise without a '$' isn't counted)

private:
    LineAssembler lines_;
    LinkState state_;
    HeadTarget head_;
    OpticalState optical_;
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

// The latest head target from a source (the link or the USB app), and
// whether targets are still arriving (LinkWatch's timeout). `seq` counts
// targets, so a reader can tell a new one from the last.
struct HeadFeed {
    LinkWatch watch;
    HeadTarget latest;
    uint32_t seq = 0;
    void got(const HeadTarget& h, uint32_t nowMs) {
        latest = h;
        ++seq;
        watch.seen(nowMs);
    }
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
