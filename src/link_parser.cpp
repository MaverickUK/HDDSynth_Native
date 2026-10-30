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

bool parseHeadSentence(const char* line, size_t len, HeadTarget& out) {
    const char* body;
    size_t bodyLen;
    if (!sentenceBody(line, len, body, bodyLen)) return false;
    Field f[5];
    const size_t n = splitFields(body, bodyLen, f, 5);
    uint32_t ver, pos, sectors;
    if (n < 5 || !f[0].is("HDE") || !f[1].toUint(ver) || ver < 2) return false;
    if (!f[2].toUint(pos) || pos > 65535 || !f[4].toUint(sectors)) return false;
    HeadTarget h;
    h.pos = static_cast<uint16_t>(pos);
    h.sectors = sectors;
    if (f[3].is("R")) h.op = LinkOp::Read;
    else if (f[3].is("W")) h.op = LinkOp::Write;
    else if (f[3].is("-")) h.op = LinkOp::None;
    else return false;
    out = h;
    return true;
}

LinkParser::Got LinkParser::feed(char c) {
    switch (lines_.feed(c)) {
        case LineAssembler::Result::None:
            return Got::Nothing;
        case LineAssembler::Result::Dropped:
            ++rejected_;
            return Got::Nothing;
        case LineAssembler::Result::Line:
            break;
    }
    LinkState s;
    if (parseLinkSentence(lines_.line(), lines_.length(), s)) {
        state_ = s;
        ++valid_;
        return Got::Activity;
    }
    HeadTarget h;
    if (parseHeadSentence(lines_.line(), lines_.length(), h)) {
        head_ = h;
        ++valid_;
        return Got::Head;
    }
    ++rejected_;
    return Got::Nothing;
}
