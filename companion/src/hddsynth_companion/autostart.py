"""Start at login, without admin rights: a LaunchAgent on macOS, the per-user Run key on
Windows. Both start this same Python with `-m hddsynth_companion`, so they keep working
after `pipx upgrade`."""

from __future__ import annotations

import os
import plistlib
import sys
from pathlib import Path

LABEL = "uk.maverick.hddsynth-companion"
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
RUN_VALUE = "HDDSynthCompanion"


def supported() -> bool:
    return sys.platform == "darwin" or sys.platform == "win32"


def _command() -> list[str]:
    exe = sys.executable
    if sys.platform == "win32":
        # pythonw.exe: no console window at login.
        w = Path(exe).with_name("pythonw.exe")
        if w.exists():
            exe = str(w)
    return [exe, "-m", "hddsynth_companion"]


def _plist() -> Path:
    return Path.home() / "Library" / "LaunchAgents" / f"{LABEL}.plist"


def enabled() -> bool:
    if sys.platform == "darwin":
        return _plist().exists()
    if sys.platform == "win32":
        import winreg

        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY) as k:
                winreg.QueryValueEx(k, RUN_VALUE)
            return True
        except OSError:
            return False
    return False


def set_enabled(on: bool) -> None:
    if sys.platform == "darwin":
        p = _plist()
        if on:
            p.parent.mkdir(parents=True, exist_ok=True)
            with open(p, "wb") as f:
                plistlib.dump({"Label": LABEL, "ProgramArguments": _command(), "RunAtLoad": True,
                               "ProcessType": "Interactive"}, f)
        elif p.exists():
            os.remove(p)
    elif sys.platform == "win32":
        import subprocess
        import winreg

        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_SET_VALUE) as k:
            if on:
                winreg.SetValueEx(k, RUN_VALUE, 0, winreg.REG_SZ, subprocess.list2cmdline(_command()))
            else:
                try:
                    winreg.DeleteValue(k, RUN_VALUE)
                except OSError:
                    pass
