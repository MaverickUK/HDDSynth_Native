// Turns audio peak levels into a smooth LED brightness. Pure C++ (no
// Arduino), host-tested.
//
// Fast attack (brightens on a beat), slower release (fades rather than
// flickers), and an auto-gain that scales to the loudest recent level, so the
// pulse uses the full brightness range whatever the recording level. The
// gain's reference decays slowly, so quieter passages still show contrast.
#pragma once

#include <math.h>
#include <stdint.h>

class LevelFollower {
public:
    LevelFollower(float attackMs, float releaseMs, float gainDecayMs, float floor)
        : attackMs_(attackMs), releaseMs_(releaseMs), gainDecayMs_(gainDecayMs), floor_(floor) {}

    // `peak` is the latest block's peak sample (0..32767), `dtMs` the time
    // since the last call. Returns brightness, floor..1.
    float update(uint16_t peak, float dtMs) {
        const float x = peak / 32767.0f;
        const float tau = x > env_ ? attackMs_ : releaseMs_;
        env_ += (x - env_) * (1.0f - expf(-dtMs / tau));
        ref_ *= expf(-dtMs / gainDecayMs_);
        if (env_ > ref_) ref_ = env_;
        // Below ~-50 dBFS treat as silence, so noise doesn't get amplified.
        const float norm = ref_ > 0.003f ? env_ / ref_ : 0.0f;
        // Squared: LED brightness looks closer to linear to the eye.
        return floor_ + (1.0f - floor_) * norm * norm;
    }

    void reset() { env_ = ref_ = 0.0f; }

private:
    float attackMs_, releaseMs_, gainDecayMs_, floor_;
    float env_ = 0.0f;
    float ref_ = 0.0f;
};
