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

bool parseOpticalSentence(const char* line, size_t len, OpticalState& out) {
    const char* body;
    size_t bodyLen;
    if (!sentenceBody(line, len, body, bodyLen)) return false;
    Field f[7];
    const size_t n = splitFields(body, bodyLen, f, 7);
    uint32_t ver;
    if (n < 6 || !f[0].is("HDO") || !f[1].toUint(ver) || ver < 3) return false;
    OpticalState o;
    if (f[2].is("1")) o.busy = true;
    else if (!f[2].is("0")) return false;
    if (f[3].is("I")) o.cls = ActivityClass::Idle;
    else if (f[3].is("S")) o.cls = ActivityClass::Sequential;
    else if (f[3].is("R")) o.cls = ActivityClass::Random;
    else return false;
    if (f[4].is("1")) o.motor = true;
    else if (!f[4].is("0")) return false;
    if (f[5].is("O")) o.trayOpen = true;
    else if (!f[5].is("C")) return false;
    if (n >= 7 && f[6].is("0")) o.disk = false;
    out = o;
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
    OpticalState o;
    if (parseOpticalSentence(lines_.line(), lines_.length(), o)) {
        optical_ = o;
        ++valid_;
        return Got::Optical;
    }
    ++rejected_;
    return Got::Nothing;
}
