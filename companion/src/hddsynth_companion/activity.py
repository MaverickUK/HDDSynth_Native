"""Turning the operating system's disk counters into HDD activity. Pure Python, no I/O.

Every poll (about 20 ms) the app samples the cumulative read/write counters of the
chosen disks. ActivityDetector works out:

* busy: more than the sensitivity threshold was transferred in the last 100 ms.
  Modern machines do a trickle of background I/O all the time; the threshold is
  what keeps that from playing the access sound non-stop.
* class: Sequential when the average transfer over the last 0.5 s was large
  (a file copy), else Random. Idle when not busy.
* op: whichever of reading and writing moved more bytes.

LinkSender then decides when to send: on any change, and every 200 ms as a
heartbeat (the device treats the app as gone after 1 s without one).
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass
from typing import Optional

BUSY_WINDOW_S = 0.1
CLASS_WINDOW_S = 0.5
SEQUENTIAL_BYTES_PER_OP = 256 * 1024
MAX_THRESHOLD_KB = 8192  # sensitivity 0: 8 MB per 100 ms (80 MB/s)
HEARTBEAT_S = 0.2


@dataclass(frozen=True)
class Sample:
    """Cumulative counters, summed over the chosen disks, at time `t` (seconds)."""

    t: float
    read_bytes: int
    write_bytes: int
    read_count: int
    write_count: int


@dataclass(frozen=True)
class Activity:
    busy: bool
    cls: str            # 'I', 'S' or 'R'
    op: str             # 'R', 'W' or '-'
    window_kb: float    # KB moved in the last BUSY_WINDOW_S (for the settings meter)
    threshold_kb: float


def threshold_kb(sensitivity: int) -> float:
    """Sensitivity 0-100 (100 = any I/O at all) to a KB-per-100-ms threshold.

    Exponential, so the slider is useful across the whole range: 100 -> 0 KB,
    60 -> ~36 KB (~0.4 MB/s), 30 -> ~550 KB, 0 -> 8 MB.
    """
    s = max(0, min(100, int(sensitivity)))
    return MAX_THRESHOLD_KB ** ((100 - s) / 100) - 1


@dataclass
class _Delta:
    t: float
    read_bytes: int
    write_bytes: int
    ops: int


class ActivityDetector:
    def __init__(self, sensitivity: int = 60) -> None:
        self.sensitivity = sensitivity
        self._last: Optional[Sample] = None
        self._deltas: deque[_Delta] = deque()

    def reset(self) -> None:
        """Forget the history (e.g. after the chosen disks change)."""
        self._last = None
        self._deltas.clear()

    def update(self, sample: Sample) -> Activity:
        last, self._last = self._last, sample
        if last is not None:
            d = _Delta(
                sample.t,
                sample.read_bytes - last.read_bytes,
                sample.write_bytes - last.write_bytes,
                (sample.read_count - last.read_count) + (sample.write_count - last.write_count),
            )
            # A disk going away (or a counter wrapping) makes a delta negative.
            if d.read_bytes >= 0 and d.write_bytes >= 0 and d.ops >= 0:
                self._deltas.append(d)
        while self._deltas and sample.t - self._deltas[0].t >= CLASS_WINDOW_S:
            self._deltas.popleft()

        recent = [d for d in self._deltas if sample.t - d.t < BUSY_WINDOW_S]
        window_bytes = sum(d.read_bytes + d.write_bytes for d in recent)
        window_ops = sum(d.ops for d in recent)
        limit = threshold_kb(self.sensitivity)
        busy = window_ops > 0 and window_bytes > limit * 1024

        if not busy:
            return Activity(False, "I", "-", window_bytes / 1024, limit)
        reads = sum(d.read_bytes for d in self._deltas)
        writes = sum(d.write_bytes for d in self._deltas)
        ops = sum(d.ops for d in self._deltas)
        sequential = ops > 0 and (reads + writes) / ops >= SEQUENTIAL_BYTES_PER_OP
        return Activity(True, "S" if sequential else "R", "W" if writes > reads else "R", window_bytes / 1024, limit)


class LinkSender:
    """When to send an $HDA sentence: on a change, and every HEARTBEAT_S regardless."""

    def __init__(self) -> None:
        self._last: Optional[tuple[bool, str, str]] = None
        self._last_t = 0.0

    def reset(self) -> None:
        self._last = None

    def should_send(self, activity: Activity, t: float) -> bool:
        key = (activity.busy, activity.cls, activity.op)
        if key != self._last or t - self._last_t >= HEARTBEAT_S:
            self._last = key
            self._last_t = t
            return True
        return False
