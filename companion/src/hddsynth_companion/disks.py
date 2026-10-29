"""Sampling the OS disk counters with psutil (IOKit on macOS, the disk performance counters
on Windows; neither needs admin rights)."""

from __future__ import annotations

import time
from typing import Iterable, Optional

import psutil

from .activity import Sample


def list_disks() -> list[str]:
    """Disk names as psutil reports them: disk0, disk4 (macOS), PhysicalDrive0 (Windows)."""
    try:
        return sorted(psutil.disk_io_counters(perdisk=True) or {})
    except Exception:  # counters can be unavailable (e.g. some VMs)
        return []


def sample(disks: Optional[Iterable[str]] = None) -> Optional[Sample]:
    """Counters summed over `disks` (None or empty: all of them), or None if unavailable."""
    try:
        counters = psutil.disk_io_counters(perdisk=True) or {}
    except Exception:
        return None
    chosen = set(disks or ())
    rb = wb = rc = wc = 0
    for name, c in counters.items():
        if chosen and name not in chosen:
            continue
        rb += c.read_bytes
        wb += c.write_bytes
        rc += c.read_count
        wc += c.write_count
    return Sample(time.monotonic(), rb, wb, rc, wc)
