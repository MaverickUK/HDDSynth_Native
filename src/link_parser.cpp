#include "link_parser.h"

#include <string.h>

bool parseLinkSentence(const char* line, size_t len, LinkState& out) {
    const char* body;
    size_t bodyLen;
    if (!sentenceBody(line, len, body, bodyLen)) return false;

    // Fields: HDA, version, busy, class, op[, later fields]. Each of the
    // first five after the name is one character.
    if (bodyLen < 11 || memcmp(body, "HDA,1,", 6) != 0) return false;
    if (bodyLen > 11 && body[11] != ',') return false;
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
    switch (lines_.feed(c)) {
        case LineAssembler::Result::None:
            return false;
        case LineAssembler::Result::Dropped:
            ++rejected_;
            return false;
        case LineAssembler::Result::Line:
            break;
    }
    LinkState s;
    if (!parseLinkSentence(lines_.line(), lines_.length(), s)) {
        ++rejected_;
        return false;
    }
    state_ = s;
    ++valid_;
    return true;
}
