// The CD-ROM drive beside the disk (CDROM_SAMPLE_PACK): which of its
// one-shot sounds plays next, and when its loops run, from what the Bay says
// the drive is doing ($HDO, link_parser.h). Platform independent (unit tested
// natively); main.cpp turns each Step into audio calls.
//
// The one-shots share one voice and play one after another, as the real
// drive makes them: an eject brakes the disc (spindown.wav) before the tray
// opens (tray_open.wav); closing it (tray_close.wav) comes before the disc
// spins up (spinup.wav). The loops (idle.wav, with access.wav and
// sequential.wav at the access level while it reads) run only while the disc
// is at speed, between a spin-up and the next spin-down.
#pragma once

#include <stdint.h>

#include "link_parser.h"

enum class CdSound : uint8_t { None, SpinUp, SpinDown, TrayOpen, TrayClose };

enum class CdPhase : uint8_t { Stopped, SpinningUp, Running, SpinningDown, TrayOpening, TrayOpen, TrayClosing };

inline const char* cdPhaseName(CdPhase p) {
    switch (p) {
        case CdPhase::Stopped: return "stopped";
        case CdPhase::SpinningUp: return "spinup";
        case CdPhase::Running: return "running";
        case CdPhase::SpinningDown: return "spindown";
        case CdPhase::TrayOpening: return "opening";
        case CdPhase::TrayOpen: return "open";
        case CdPhase::TrayClosing: return "closing";
    }
    return "?";
}

class OpticalPlayer {
public:
    struct Step {
        CdSound start = CdSound::None;  // start this one-shot (replacing any still playing)
        bool startLoops = false;        // the disc is at speed: start the loops
        bool stopLoops = false;         // it's slowing: stop them
    };

    // `want`: what the Bay says (all off when the link is gone);
    // `oneShotPlaying`: whether the last one-shot started is still playing (a
    // sound that couldn't start counts as finished, so a pack without one
    // just moves on).
    Step step(const OpticalState& want, bool oneShotPlaying) {
        Step s;
        // Twice at most: a finished sound can lead straight to the next.
        for (int pass = 0; pass < 2; ++pass) {
            const bool stop = want.trayOpen || !want.motor;
            switch (phase_) {
                case CdPhase::Stopped:
                    if (want.trayOpen) return begin(s, CdSound::TrayOpen, CdPhase::TrayOpening);
                    if (want.motor) return begin(s, CdSound::SpinUp, CdPhase::SpinningUp);
                    return s;
                case CdPhase::SpinningUp:
                    if (stop) return begin(s, CdSound::SpinDown, CdPhase::SpinningDown);
                    if (!oneShotPlaying) {
                        s.startLoops = true;
                        phase_ = CdPhase::Running;
                    }
                    return s;
                case CdPhase::Running:
                    if (stop) {
                        s.stopLoops = true;
                        return begin(s, CdSound::SpinDown, CdPhase::SpinningDown);
                    }
                    return s;
                case CdPhase::SpinningDown:
                case CdPhase::TrayClosing:
                    if (oneShotPlaying) return s;
                    phase_ = CdPhase::Stopped;
                    oneShotPlaying = false;
                    continue;  // the drive's next move, if any
                case CdPhase::TrayOpening:
                    if (!oneShotPlaying) phase_ = CdPhase::TrayOpen;
                    if (phase_ == CdPhase::TrayOpening) return s;
                    continue;
                case CdPhase::TrayOpen:
                    if (!want.trayOpen) return begin(s, CdSound::TrayClose, CdPhase::TrayClosing);
                    return s;
            }
        }
        return s;
    }

    CdPhase phase() const { return phase_; }
    bool running() const { return phase_ == CdPhase::Running; }
    // Back to silence (the voices were stopped: a pack change, power loss).
    void reset() { phase_ = CdPhase::Stopped; }

private:
    Step& begin(Step& s, CdSound sound, CdPhase next) {
        s.start = sound;
        phase_ = next;
        return s;
    }

    CdPhase phase_ = CdPhase::Stopped;
};
