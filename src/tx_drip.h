// One outgoing sentence for a UART, fed to it a byte at a time while its
// transmit FIFO has room, so the sender never waits: the link's $HDM replies
// to HDDSynthPro (hdd_link.cpp).
//
// Arduino-Pico's SerialUART::availableForWrite() says only whether one byte
// fits (1 or 0), never how many, so "send when the whole sentence fits" never
// sent at all (v0.24.0: `sent=0`, HDDSynthPro's `hdm_valid=0`). And a model
// sentence (about 46 bytes) is longer than the 32-byte hardware FIFO anyway.
// Pure C++, host-tested.
#pragma once

#include <cstddef>
#include <cstring>

class TxDrip {
public:
    static constexpr size_t CAPACITY = 96;

    // True while a sentence is still going out.
    bool busy() const { return pos_ < len_; }

    // Takes a sentence to send; false (and nothing changes) while the last is
    // still going out, or if it's empty or too long. The caller sends the
    // next one later (the model is resent every 2 s).
    bool start(const char* text, size_t len) {
        if (busy() || len == 0 || len > CAPACITY) return false;
        std::memcpy(buf_, text, len);
        len_ = len;
        pos_ = 0;
        return true;
    }

    // Writes bytes while `room()` says one fits; returns how many it wrote.
    template <class Room, class Put>
    size_t drain(Room room, Put put) {
        size_t n = 0;
        while (busy() && room()) {
            put(buf_[pos_++]);
            ++n;
        }
        return n;
    }

private:
    char buf_[CAPACITY] = {};
    size_t len_ = 0, pos_ = 0;
};
