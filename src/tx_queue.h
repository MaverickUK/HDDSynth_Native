// A byte queue for sentences waiting to go out of a UART whose hardware FIFO
// is smaller than they are (the HDDSynthPro link: 32 bytes, where an $HDP
// line is up to about 85). Whole sentences go in or none of it, so lines
// never interleave; the caller drains it into the FIFO as room appears.
// Single core, no locking. Platform independent (unit tested natively).
#pragma once

#include <stddef.h>
#include <stdint.h>

class TxQueue {
public:
    // `storage` holds the bytes; the queue never allocates.
    TxQueue(char* storage, size_t capacity) : buf_(storage), cap_(capacity) {}

    size_t size() const { return size_; }
    size_t room() const { return cap_ - size_; }
    bool empty() const { return size_ == 0; }

    // All `n` bytes, or none if there isn't room for all of them.
    bool push(const char* p, size_t n) {
        if (n > room()) return false;
        for (size_t i = 0; i < n; ++i) buf_[(head_ + size_ + i) % cap_] = p[i];
        size_ += n;
        return true;
    }

    // The next byte, removed. Only when !empty().
    char pop() {
        const char c = buf_[head_];
        head_ = (head_ + 1) % cap_;
        --size_;
        return c;
    }

private:
    char* buf_;
    size_t cap_;
    size_t head_ = 0;
    size_t size_ = 0;
};
