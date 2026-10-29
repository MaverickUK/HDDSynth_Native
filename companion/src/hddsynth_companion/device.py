"""Finding the HDD Synth on USB and talking to it. No GUI code: the app wires the
callbacks to Qt signals.

The board enumerates as a USB serial port (no driver needed): Raspberry Pi's VID with
the product name "HDDSYNTH". Windows' built-in driver usually hides that name ("USB Serial
Device (COM5)"), so any port with the Raspberry Pi VID is a candidate too: the app keeps
one only if it answers a status query like an HDD Synth. While this app has the port open
the board's human serial console can't be used from a terminal.
"""

from __future__ import annotations

import threading
from typing import Callable, Iterable, Optional

from . import protocol

VID = 0x2E8A
PRODUCT = "HDDSYNTH"
BAUD = 115200


def matches(port) -> bool:
    """Whether a pyserial ListPortInfo looks like an HDD Synth."""
    if port.vid != VID:
        return False
    names = f"{port.product or ''} {port.manufacturer or ''} {port.description or ''}".upper()
    return PRODUCT in names or "HDD SYNTH" in names


def candidate_ports(ports: Optional[Iterable] = None) -> list[str]:
    """Ports that may be an HDD Synth: those named as one first, then any other port with
    the Raspberry Pi VID (to be confirmed by a status query)."""
    if ports is None:
        from serial.tools import list_ports

        ports = list_ports.comports()
    ports = list(ports)
    named = [p.device for p in ports if matches(p)]
    others = [p.device for p in ports if p.vid == VID and p.device not in named]
    return named + others


def find_port(ports: Optional[Iterable] = None) -> Optional[str]:
    """The first port named as an HDD Synth (no status query needed)."""
    if ports is None:
        from serial.tools import list_ports

        ports = list_ports.comports()
    for p in ports:
        if matches(p):
            return p.device
    return None


def all_ports() -> list[str]:
    from serial.tools import list_ports

    return sorted(p.device for p in list_ports.comports())


class DeviceLink:
    """One connection to the board. connect() opens it and starts a reader thread;
    `on_reply` is called (from that thread) for every device sentence, and
    `on_closed` once when the connection ends."""

    def __init__(self, on_reply: Callable[[protocol.Reply], None], on_closed: Callable[[], None]) -> None:
        self._on_reply = on_reply
        self._on_closed = on_closed
        self._serial = None
        self._lock = threading.Lock()
        self.port: Optional[str] = None

    @property
    def connected(self) -> bool:
        return self._serial is not None

    def connect(self, port: str) -> bool:
        import serial

        try:
            # A short timeout lets the reader thread notice close().
            s = serial.Serial(port, BAUD, timeout=0.2, write_timeout=0.5)
        except (serial.SerialException, OSError):
            return False
        self._serial = s
        self.port = port
        threading.Thread(target=self._read_loop, args=(s,), name="hddsynth-reader", daemon=True).start()
        return True

    def close(self) -> None:
        with self._lock:
            s, self._serial = self._serial, None
        if s is not None:
            try:
                s.close()
            except Exception:
                pass

    def send(self, data: bytes) -> bool:
        with self._lock:
            s = self._serial
            if s is None:
                return False
            try:
                s.write(data)
                return True
            except Exception:
                pass
        self._lost()
        return False

    def _lost(self) -> None:
        if self._serial is not None:
            self.close()
            self._on_closed()

    def _read_loop(self, s) -> None:
        buf = b""
        while self._serial is s:
            try:
                chunk = s.read(512)
            except Exception:
                break
            if not chunk:
                continue
            buf += chunk
            *lines, buf = buf.split(b"\n")
            for raw in lines:
                reply = protocol.parse_reply(raw.decode("ascii", "replace"))
                if reply is not None:
                    self._on_reply(reply)
            if len(buf) > 4096:  # a log line without an end: don't grow forever
                buf = b""
        if self._serial is s:
            self._lost()
