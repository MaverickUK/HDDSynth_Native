"""The app's own settings, as JSON in the user's config folder:
~/Library/Application Support/hddsynth-companion (macOS), %LOCALAPPDATA%\\hddsynth-companion
(Windows)."""

from __future__ import annotations

import json
from dataclasses import asdict, dataclass, field, fields
from pathlib import Path

from platformdirs import user_config_dir

APP_NAME = "hddsynth-companion"


@dataclass
class Config:
    sensitivity: int = 60                            # 0-100, see activity.threshold_kb()
    disks: list[str] = field(default_factory=list)   # empty: all disks
    port: str = ""                                   # empty: find the HDD Synth automatically
    send_activity: bool = True                       # off: controls only, no disk activity


def path() -> Path:
    return Path(user_config_dir(APP_NAME, appauthor=False)) / "config.json"


def load() -> Config:
    try:
        data = json.loads(path().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return Config()
    known = {f.name for f in fields(Config)}
    cfg = Config(**{k: v for k, v in data.items() if k in known})
    cfg.sensitivity = max(0, min(100, int(cfg.sensitivity)))
    return cfg


def save(cfg: Config) -> None:
    p = path()
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_suffix(".tmp")
    tmp.write_text(json.dumps(asdict(cfg), indent=2), encoding="utf-8")
    tmp.replace(p)
