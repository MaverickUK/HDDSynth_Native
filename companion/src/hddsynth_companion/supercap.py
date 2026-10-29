"""How much the supercap can do: the colours and marks of the settings window's supercap
bar. Pure Python.

The thresholds are ESTIMATES until a real power cut is measured (the firmware's power
log records the voltage through the spindown). The sums, for the 1 F supercap:

* usable energy from V down to where the board stops (~1.1 V in the one real cut):
  E = 0.5 * C * (V^2 - 1.1^2)
* a spindown drawing ~0.5 W through the boost converter (~85% efficient):
  typical 5 s  -> ~2.9 J -> V >= ~2.7 V
  longest 11 s -> ~6.5 J -> V >= ~3.8 V  (PB_MicroScience612)

Below SPINDOWN_TYPICAL_MV a spindown may not finish (red); up to SPINDOWN_LONGEST_MV a
typical one will but the longest may not (orange); above that, any (green).
"""

from __future__ import annotations

SCALE_MIN_MV = 1000          # the board doesn't run below about this
SCALE_MAX_MV = 4200          # the boost target; the charger stops at 4.15 V
SPINDOWN_TYPICAL_MV = 2700   # estimate: enough for a typical (~5 s) spindown
SPINDOWN_LONGEST_MV = 3800   # estimate: enough for the longest (~11 s)

RED, ORANGE, GREEN = "red", "orange", "green"


def level(mv: int) -> str:
    if mv < SPINDOWN_TYPICAL_MV:
        return RED
    if mv < SPINDOWN_LONGEST_MV:
        return ORANGE
    return GREEN


def fraction(mv: int) -> float:
    """Position along the bar, 0..1."""
    f = (mv - SCALE_MIN_MV) / (SCALE_MAX_MV - SCALE_MIN_MV)
    return max(0.0, min(1.0, f))


def describe(mv: int) -> str:
    """A tooltip."""
    return {
        RED: "Not enough charge yet for a spindown on a power cut.",
        ORANGE: "Enough charge for a typical spindown; not yet for the longest.",
        GREEN: "Charged: enough for any pack's spindown.",
    }[level(mv)]
