#include "link_parser.h"

#include <string.h>

namespace {

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

} // namespace

bool parseLinkSentence(const char* line, size_t len, LinkState& out) {
    // "$HDA,1,b,c,o*hh" is the shortest valid sentence.
    if (len < 15 || line[0] != '$') return false;
    const char* star = static_cast<const char*>(memchr(line, '*', len));
    if (!star || line + len - star != 3) return false;  // exactly two digits after '*'
    const int hi = hexDigit(star[1]), lo = hexDigit(star[2]);
    if (hi < 0 || lo < 0) return false;
    uint8_t sum = 0;
    for (const char* p = line + 1; p < star; ++p) sum ^= static_cast<uint8_t>(*p);
    if (sum != (hi << 4 | lo)) return false;

    // Fields between '$' and '*': HDA, version, busy, class, op[, later fields].
    const char* body = line + 1;
    const size_t bodyLen = static_cast<size_t>(star - body);
    if (bodyLen < 11 || memcmp(body, "HDA,1,", 6) != 0) return false;
    if (bodyLen > 11 && body[11] != ',') return false;  // each field is one character
    if (body[7] != ',' || body[9] != ',') return false;

    LinkState s;
    switch (body[6]) {
        case '0': s.busy = false; break;
        case '1': s.busy = true; break;
        default: return false;
    }
    switch (body[8]) {
        case 'I': s.cls = ActivityClass::Idle; break;
        case 'S': s.cls = ActivityClass::Sequential; break;
        case 'R': s.cls = ActivityClass::Random; break;
        default: return false;
    }
    switch (body[10]) {
        case '-': s.op = LinkOp::None; break;
        case 'R': s.op = LinkOp::Read; break;
        case 'W': s.op = LinkOp::Write; break;
        default: return false;
    }
    out = s;
    return true;
}

bool LinkParser::feed(char c) {
    if (c == '$') {  // a new sentence, whatever came before
        if (inLine_) ++rejected_;  // the previous one never ended
        inLine_ = true;
        tooLong_ = false;
        buf_[0] = c;
        len_ = 1;
        return false;
    }
    if (!inLine_) return false;  // noise between sentences
    if (c == '\n') {
        inLine_ = false;
        size_t n = len_;
        if (n && buf_[n - 1] == '\r') --n;
        LinkState s;
        if (!tooLong_ && n <= MAX_LINE && parseLinkSentence(buf_, n, s)) {
            state_ = s;
            ++valid_;
            return true;
        }
        ++rejected_;
        return false;
    }
    if (len_ < sizeof(buf_)) buf_[len_++] = c;
    else tooLong_ = true;
    return false;
}
