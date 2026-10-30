// The USB companion app protocol (companion/): '$' sentences over the USB
// serial console, in the same format as the HDDSynthPro link (sentence.h).
//
//   app -> device   $HDA,1,<busy>,<class>,<op>   disk activity (link_parser.h)
//                   $HDQ,1,STATUS | $HDQ,1,PACKS | $HDQ,1,MODEL  queries
//                   $HDE,2,<pos>,<op>,<sectors>  a command's target (link_parser.h)
//                   $HDC,1,VOL,<0-100> | $HDC,1,BAL,<0-100> | $HDC,1,PACK,<index>
//   device -> app   $HDS,1,<fw>,<state>,<packIndex>,<packCount>,<vol>,<bal>,<supercapMv>
//                   $HDP,1,<index>,<count>,<folder>,<name>   one per pack
//                   $HDK,1,<VOL|BAL|PACK>,<OK|ERR>,<reason>
//                   $HDM,2,...                   the modelled drive's timing (drive_timing.h)
//
// Every sentence ends *hh (checksum). The README describes it in full.
// Platform independent (unit tested natively).
#pragma once

#include <stddef.h>
#include <stdint.h>

enum class HostCommandType : uint8_t { Status, Packs, Volume, Balance, Pack, Model };

struct HostCommand {
    HostCommandType type = HostCommandType::Status;
    uint32_t value = 0;  // percent for Volume/Balance, the pack index for Pack
};

enum class HostParse : uint8_t {
    Command,     // `out` holds a command
    NotCommand,  // a valid sentence, but not $HDQ/$HDC (e.g. $HDA)
    Invalid,     // bad checksum, unknown command or value out of range
};

// Parse one '$' line, without its line ending.
HostParse parseHostCommand(const char* line, size_t len, HostCommand& out);

// The command's name in $HDK replies ("VOL", "BAL", "PACK", ...).
const char* hostCommandName(HostCommandType t);

// Copy a pack's folder or display name for a sentence field: the
// characters that would break the sentence (',', '*', '$') and control
// characters become '_'. Always NUL-terminates `out`.
void sanitiseName(const char* in, char* out, size_t outLen);
