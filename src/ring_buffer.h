// Lock-free single-producer / single-consumer ring buffer of audio samples.
//
// Producer: core 0 (SD streamer). Consumer: core 1 (mixer).
// Indices are free-running 32-bit counters; capacity must be a power of two.
// reset() may only be called while the consumer is guaranteed not to touch
// the ring (its voice is IDLE), see audio_engine.h.
#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

template <typename T, size_t N>
class SpscRing {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "capacity must be a power of two");

public:
    static constexpr size_t capacity() { return N; }

    size_t size() const {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }
    size_t free() const { return N - size(); }

    // Producer side. Returns number of items written.
    size_t push(const T* src, size_t count) {
        const uint32_t head = head_.load(std::memory_order_relaxed);
        const uint32_t tail = tail_.load(std::memory_order_acquire);
        const size_t space = N - (head - tail);
        if (count > space) count = space;
        copy_in(head, src, count);
        head_.store(head + static_cast<uint32_t>(count), std::memory_order_release);
        return count;
    }

    // Consumer side. Returns number of items read.
    size_t pop(T* dst, size_t count) {
        const uint32_t tail = tail_.load(std::memory_order_relaxed);
        const uint32_t head = head_.load(std::memory_order_acquire);
        const size_t avail = head - tail;
        if (count > avail) count = avail;
        copy_out(tail, dst, count);
        tail_.store(tail + static_cast<uint32_t>(count), std::memory_order_release);
        return count;
    }

    void reset() {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_release);
    }

private:
    void copy_in(uint32_t head, const T* src, size_t count) {
        const size_t start = head & (N - 1);
        const size_t first = (count < N - start) ? count : N - start;
        memcpy(&buf_[start], src, first * sizeof(T));
        if (count > first) memcpy(&buf_[0], src + first, (count - first) * sizeof(T));
    }
    void copy_out(uint32_t tail, T* dst, size_t count) const {
        const size_t start = tail & (N - 1);
        const size_t first = (count < N - start) ? count : N - start;
        memcpy(dst, &buf_[start], first * sizeof(T));
        if (count > first) memcpy(dst + first, &buf_[0], (count - first) * sizeof(T));
    }

    T buf_[N];
    std::atomic<uint32_t> head_{0};
    std::atomic<uint32_t> tail_{0};
};
