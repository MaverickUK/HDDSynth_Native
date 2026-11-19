#!/usr/bin/env python3
"""Builds Drive Lab v2 from page.html, engine.js and the drives.

    python3 tools/drive-lab-v2/build.py              ->  tools/drive-lab-v2/drive-lab.html
    python3 tools/drive-lab-v2/build.py --site DIR   ->  DIR/index.html, a full web page

The plain build is the page's body (as v1's, for a claude.ai artifact, which adds the
document skeleton); --site wraps it in a full HTML document. The presets are
drives/designed_*.json and drives/fitted/*.json, embedded.
"""
import json
import sys
from pathlib import Path

here = Path(__file__).resolve().parent
drives = {}
for f in sorted((here / "drives").glob("designed_*.json")):
    drives["designed/" + f.stem] = json.loads(f.read_text())
for f in sorted((here / "drives" / "fitted").glob("*.json")):
    drives["fitted/" + f.stem] = json.loads(f.read_text())

page = (
    (here / "page.html").read_text(encoding="utf-8")
    .replace("/*ENGINE*/", (here / "engine.js").read_text(encoding="utf-8"))
    .replace("/*DRIVES*/", json.dumps(drives, separators=(",", ":")))
)

SITE_HEAD = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="description" content="Design the sound of a mechanical hard drive, synthesised live in your browser.">
<style>
body { margin: 0; font: 14px/1.45 system-ui, sans-serif; }
[hidden] { display: none !important; }
</style>
"""

if len(sys.argv) == 3 and sys.argv[1] == "--site":
    out_dir = Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)
    split = page.index('<div class="wrap">')
    out = out_dir / "index.html"
    out.write_text(SITE_HEAD + page[:split] + "</head>\n<body>\n" + page[split:] + "</body>\n</html>\n", encoding="utf-8")
elif len(sys.argv) == 1:
    out = here / "drive-lab.html"
    out.write_text(page, encoding="utf-8")
else:
    sys.exit("usage: build.py [--site DIR]")
print(f"wrote {out}")
