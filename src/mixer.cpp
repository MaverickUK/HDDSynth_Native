#include "mixer.h"

#include <string.h>

namespace {

// Fill `dst` with up to `frames` samples from the voice's source.
// Returns false when the source has finished (dst is zero padded).
bool readSource(Voice& v, int16_t* dst, size_t frames) {
    if (v.beep) {
        for (size_t n = 0; n < frames; ++n) dst[n] = beepSample(*v.beep, v.memPos + static_cast<uint32_t>(n));
        v.memPos += static_cast<uint32_t>(frames);
        return v.memPos < v.beep->totalSamples;
    }
    if (v.mem) {
        size_t done = 0;
        while (done < frames) {
            if (v.memPos >= v.memLen) {
                if (!v.loop || v.memLen == 0) {
                    memset(dst + done, 0, (frames - done) * sizeof(int16_t));
                    return false;
                }
                v.memPos = 0;
            }
            size_t n = v.memLen - v.memPos;
            if (n > frames - done) n = frames - done;
            memcpy(dst + done, v.mem + v.memPos, n * sizeof(int16_t));
            v.memPos += static_cast<uint32_t>(n);
            done += n;
        }
        // A non-looping sample that ends exactly on a block boundary finishes next block.
        return true;
    }

    if (!v.ring) {
        memset(dst, 0, frames * sizeof(int16_t));
        return false;
    }

    // Load eof BEFORE popping: if it was already set, every sample the
    // producer will ever push is visible, so a short pop means "finished".
    const bool producerDone = v.eof.load(std::memory_order_acquire);
    const size_t fill = v.ring->size();
    // Track headroom only while the producer is still feeding: a stream that
    // is draining at its end legitimately runs the ring down to zero.
    if (!producerDone && fill < v.minRingFill.load(std::memory_order_relaxed)) {
        v.minRingFill.store(static_cast<uint32_t>(fill), std::memory_order_relaxed);
    }
    const size_t got = v.ring->pop(dst, frames);
    if (got < frames) {
        memset(dst + got, 0, (frames - got) * sizeof(int16_t));
        if (producerDone) return false;
        v.underrunSamples.fetch_add(static_cast<uint32_t>(frames - got), std::memory_order_relaxed);
    }
    return true;
}

} // namespace

void mixBlock(Voice* voices, size_t voiceCount, int16_t* out, size_t frames) {
    int32_t acc[cfg::MIX_BLOCK_FRAMES];
    int16_t tmp[cfg::MIX_BLOCK_FRAMES];
    if (frames > cfg::MIX_BLOCK_FRAMES) frames = cfg::MIX_BLOCK_FRAMES;
    memset(acc, 0, frames * sizeof(int32_t));

    for (size_t i = 0; i < voiceCount; ++i) {
        Voice& v = voices[i];
        const uint8_t st = v.state.load(std::memory_order_acquire);
        if (st == VS_STOP_REQ) {
            v.state.store(VS_IDLE, std::memory_order_release);
            continue;
        }
        if (st != VS_PLAYING) continue;

        const uint32_t gen = v.playGen.load(std::memory_order_acquire);
        if (gen != v.seenGen) {
            // A fresh start: rewind memory sources and jump straight to the level.
            v.seenGen = gen;
            v.memPos = 0;
            v.curLevel = v.targetLevel.load(std::memory_order_relaxed);
        }

        const bool more = readSource(v, tmp, frames);

        // Linear ramp from the current to the target level across the block
        // so level changes (access on/off, volume) never click.
        const int32_t from = v.curLevel;
        const int32_t to = v.targetLevel.load(std::memory_order_relaxed);
        if (from == to) {
            if (from != 0) {
                for (size_t n = 0; n < frames; ++n) acc[n] += (int32_t(tmp[n]) * from) >> 15;
            }
        } else {
            const int32_t delta = to - from;
            for (size_t n = 0; n < frames; ++n) {
                const int32_t g = from + (delta * int32_t(n + 1)) / int32_t(frames);
                acc[n] += (int32_t(tmp[n]) * g) >> 15;
            }
            v.curLevel = static_cast<uint16_t>(to);
        }

        if (!more) v.state.store(VS_IDLE, std::memory_order_release);
    }

    for (size_t n = 0; n < frames; ++n) {
        int32_t s = acc[n];
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        out[n] = static_cast<int16_t>(s);
    }
}
