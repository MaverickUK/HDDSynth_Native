#!/usr/bin/env python3
"""Builds the Drive Lab (the modelled drive's browser prototype) from page.html and engine.js.

    python3 tools/drive-lab/build.py              ->  tools/drive-lab/drive-lab.html
    python3 tools/drive-lab/build.py --site DIR   ->  DIR/index.html, the website (GitHub Pages)

The plain build is the page's body, for publishing as a claude.ai artifact (which adds the
document skeleton). The site build wraps it in a full HTML document with the base styles the
artifact skeleton would provide, a favicon, and the website-only features (downloading drive.json).
"""
import sys
from pathlib import Path

here = Path(__file__).resolve().parent
page = (here / "page.html").read_text().replace("/*ENGINE*/", (here / "engine.js").read_text())

# A platter and arm, for the browser tab.
FAVICON = (
    "data:image/svg+xml,"
    "%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'%3E"
    "%3Crect width='32' height='32' rx='6' fill='%231A1E23'/%3E"
    "%3Ccircle cx='13' cy='16' r='10' fill='%23BFC5CB'/%3E%3Ccircle cx='13' cy='16' r='2.5' fill='%231A1E23'/%3E"
    "%3Cpath d='M27 27 L15 10' stroke='%23EE8A3C' stroke-width='3' stroke-linecap='round'/%3E%3C/svg%3E"
)

SITE_HEAD = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="description" content="Design the sound of a mechanical hard drive: a physically modelled drive for the HDD Synth, synthesised live in your browser.">
<link rel="icon" href="{FAVICON}">
<style>
:root {{ color-scheme: light; padding-top: env(safe-area-inset-top, 0px); padding-bottom: env(safe-area-inset-bottom, 0px); }}
body {{ margin: 0; font: 14px/1.45 system-ui, sans-serif; }}
img {{ max-width: 100%; }}
[hidden] {{ display: none !important; }}
</style>
<script>window.DRIVE_LAB_SITE = true;</script>
"""

if len(sys.argv) == 3 and sys.argv[1] == "--site":
    out_dir = Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)
    # The page's own <title>, fonts and styles stay at its top; in a full document they belong
    # in the head, and the rest in the body.
    split = page.index('<div class="wrap">')
    out = out_dir / "index.html"
    out.write_text(SITE_HEAD + page[:split] + "</head>\n<body>\n" + page[split:] + "</body>\n</html>\n")
elif len(sys.argv) == 1:
    out = here / "drive-lab.html"
    out.write_text(page)
else:
    sys.exit("usage: build.py [--site DIR]")
print(f"wrote {out}")
