"""The HDD Synth's USB protocol: NMEA-style '$' sentences over its serial console.

    $<NAME>,<field>,...*<checksum>\\r\\n      (checksum: XOR of the bytes between '$' and '*')

App to device:
    $HDA,1,<busy 0|1>,<class I|S|R>,<op R|W|->   disk activity (on change and every 200 ms)
    $HDQ,1,STATUS / $HDQ,1,PACKS                  queries
    $HDC,1,VOL,<0-100> / BAL,<0-100> / PACK,<i>   commands
Device to app:
    $HDS,1,<fw>,<state>,<packIndex>,<packCount>,<vol>,<bal>,<supercapMv>
    $HDP,1,<index>,<count>,<folder>,<name>
    $HDK,1,<command>,<OK|ERR>,<reason>

This mirrors the firmware's src/host_protocol.h and src/sentence.h. Pure Python, no I/O.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Optional, Union

VERSION = "1"


def checksum(body: str) -> int:
    value = 0
    for b in body.encode("ascii", "replace"):
        value ^= b
    return value


def format_sentence(body: str) -> bytes:
    """`body` is everything between '$' and '*'."""
    return f"${body}*{checksum(body):02X}\r\n".encode("ascii", "replace")


def parse_sentence(line: str) -> Optional[list[str]]:
    """The fields of a valid sentence (line endings optional), or None."""
    line = line.strip()
    if len(line) < 4 or not line.startswith("$"):
        return None
    star = line.find("*")
    if star < 0 or len(line) - star != 3:
        return None
    try:
        expected = int(line[star + 1 :], 16)
    except ValueError:
        return None
    body = line[1:star]
    if checksum(body) != expected:
        return None
    return body.split(",")


# ---- app to device ----------------------------------------------------------


def activity(busy: bool, cls: str, op: str) -> bytes:
    """cls: 'I', 'S' or 'R'; op: 'R', 'W' or '-'."""
    return format_sentence(f"HDA,{VERSION},{int(busy)},{cls},{op}")


def query_status() -> bytes:
    return format_sentence(f"HDQ,{VERSION},STATUS")


def query_packs() -> bytes:
    return format_sentence(f"HDQ,{VERSION},PACKS")


def _percent(value: int) -> int:
    return max(0, min(100, int(value)))


def set_volume(percent: int) -> bytes:
    return format_sentence(f"HDC,{VERSION},VOL,{_percent(percent)}")


def set_balance(percent: int) -> bytes:
    return format_sentence(f"HDC,{VERSION},BAL,{_percent(percent)}")


def set_pack(index: int) -> bytes:
    return format_sentence(f"HDC,{VERSION},PACK,{int(index)}")


# ---- device to app ----------------------------------------------------------


@dataclass(frozen=True)
class Status:
    firmware: str
    state: str          # RUN, SPINUP, STANDBY, HALTED, ...
    pack_index: int     # -1 if unknown
    pack_count: int
    volume: int         # percent
    balance: int        # percent: 0 = all idle/spinup/spindown, 100 = all access
    supercap_mv: int


@dataclass(frozen=True)
class PackEntry:
    index: int
    count: int
    folder: str
    name: str           # pack.json NAME, or the folder name


@dataclass(frozen=True)
class Ack:
    command: str        # VOL, BAL or PACK
    ok: bool
    reason: str         # e.g. "busy", "demo", "range"; "" when ok


Reply = Union[Status, PackEntry, Ack]


def parse_reply(line: str) -> Optional[Reply]:
    """A device sentence, or None for anything else (the human console's log lines)."""
    f = parse_sentence(line)
    if not f or len(f) < 2 or f[1] != VERSION:
        return None
    try:
        if f[0] == "HDS" and len(f) >= 9:
            return Status(f[2], f[3], int(f[4]), int(f[5]), int(f[6]), int(f[7]), int(f[8]))
        if f[0] == "HDP" and len(f) >= 6:
            return PackEntry(int(f[2]), int(f[3]), f[4], f[5])
        if f[0] == "HDK" and len(f) >= 5:
            return Ack(f[2], f[3] == "OK", f[4])
    except ValueError:
        return None
    return None
