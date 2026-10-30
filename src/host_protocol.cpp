#include "host_protocol.h"

#include "sentence.h"

HostParse parseHostCommand(const char* line, size_t len, HostCommand& out) {
    const char* body;
    size_t bodyLen;
    if (!sentenceBody(line, len, body, bodyLen)) return HostParse::Invalid;
    Field f[4];
    const size_t n = splitFields(body, bodyLen, f, 4);
    const bool query = f[0].is("HDQ"), command = f[0].is("HDC");
    if (!query && !command) return HostParse::NotCommand;
    if (n < 3 || !f[1].is("1")) return HostParse::Invalid;

    HostCommand c;
    if (query) {
        if (f[2].is("STATUS")) c.type = HostCommandType::Status;
        else if (f[2].is("PACKS")) c.type = HostCommandType::Packs;
        else if (f[2].is("MODEL")) c.type = HostCommandType::Model;
        else return HostParse::Invalid;
    } else {
        if (n < 4 || !f[3].toUint(c.value)) return HostParse::Invalid;
        if (f[2].is("VOL")) c.type = HostCommandType::Volume;
        else if (f[2].is("BAL")) c.type = HostCommandType::Balance;
        else if (f[2].is("PACK")) c.type = HostCommandType::Pack;  // the caller checks it against the pack count
        else return HostParse::Invalid;
        if (c.type != HostCommandType::Pack && c.value > 100) return HostParse::Invalid;
    }
    out = c;
    return HostParse::Command;
}

const char* hostCommandName(HostCommandType t) {
    switch (t) {
        case HostCommandType::Status: return "STATUS";
        case HostCommandType::Packs: return "PACKS";
        case HostCommandType::Volume: return "VOL";
        case HostCommandType::Balance: return "BAL";
        case HostCommandType::Pack: return "PACK";
        case HostCommandType::Model: return "MODEL";
    }
    return "?";
}

void sanitiseName(const char* in, char* out, size_t outLen) {
    if (!outLen) return;
    size_t i = 0;
    for (; in[i] && i + 1 < outLen; ++i) {
        const char c = in[i];
        out[i] = (c == ',' || c == '*' || c == '$' || static_cast<unsigned char>(c) < 0x20) ? '_' : c;
    }
    out[i] = 0;
}
