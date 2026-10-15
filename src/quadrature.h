// Rotary encoder quadrature decoding. Pure C++ (no Arduino), host-tested.
//
// The encoder's two switches (A and B) produce a 2-bit Gray code: one detent
// (click) is one full cycle of four transitions, resting with both switches
// open (A = B = 1 with pull-ups), as CircuitPython's rotaryio assumes.
//
// Each transition moves a quarter-step counter by +1 or -1; contact bounce
// moves it back and forth and cancels out. A detent is only reported when
// the encoder comes back to rest, and then by the sign of the counter, so a
// missed transition (from a fast turn) can't leave the count misaligned.
#pragma once

#include <stdint.h>

class QuadratureDecoder {
public:
    static constexpr uint8_t REST = 0b11;  // A and B both high: sitting in a detent

    explicit QuadratureDecoder(uint8_t initialAB = REST) : prev_(initialAB & 3) {}

    // Feed the current A/B state (bit 1 = A, bit 0 = B). Returns +1 or -1 when
    // a detent completes, otherwise 0.
    int update(uint8_t ab) {
        ab &= 3;
        // Indexed by (previous << 2) | current. 0 = no change or an invalid
        // jump (both bits changed: a transition was missed).
        static constexpr int8_t STEP[16] = {0, -1, +1, 0, +1, 0, 0, -1, -1, 0, 0, +1, 0, +1, -1, 0};
        quarter_ += STEP[(prev_ << 2) | ab];
        prev_ = ab;
        if (ab != REST) return 0;
        // Back at rest: a full turn is +/-4 quarters; accept 2 or more in case
        // transitions were missed, and treat less as bounce.
        const int result = quarter_ >= 2 ? +1 : quarter_ <= -2 ? -1 : 0;
        quarter_ = 0;
        return result;
    }

private:
    uint8_t prev_;
    int8_t quarter_ = 0;
};
