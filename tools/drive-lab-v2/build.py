#!/usr/bin/env python3
"""Builds the Drive Lab page from page.html, the JS modules and the fitted drives.

    python3 tools/drive-lab-v2/build.py              ->  tools/drive-lab-v2/drive-lab.html
    python3 tools/drive-lab-v2/build.py --site DIR   ->  DIR/index.html, a full web page

The plain build is the page's body (as v1's, for a claude.ai artifact, which adds the
document skeleton); --site wraps it in a full HTML document.

The page runs the same code as the Node tools: each module is wrapped in a function
inside one __bundle() (a tiny CommonJS: require, module.exports), which returns
require. The page calls it, and so does the fitting worker (from its source text),
as the AudioWorklet does with makeEngine. fs and path are stubs: the browser path
never touches files.

The fitted examples are drives/fitted/*.json, embedded.
"""
import json
import sys
from pathlib import Path

here = Path(__file__).resolve().parent
MODULES = ["dsp.js", "engine.js", "render.js", "wav.js", "seek_fit.js", "spindown_fit.js",
           "head_fit.js", "fit.js", "grains.js", "design.js"]

parts = ["""function __bundle() {
  const defs = {}, cache = {};
  const stub = { fs: {}, path: { join: (...a) => a.join('/'), basename: (s) => String(s).split('/').pop(),
    dirname: (s) => String(s).split('/').slice(0, -1).join('/'), resolve: (s) => s } };
  function require(n) {
    if (stub[n]) return stub[n];
    n = n.replace(/^\\.\\//, '');
    if (cache[n]) return cache[n].exports;
    if (!defs[n]) throw new Error('no module ' + n);
    const module = { exports: {} };
    cache[n] = module;
    defs[n](module, module.exports, require);
    return module.exports;
  }
"""]
for m in MODULES:
    src = (here / m).read_text(encoding="utf-8")
    if "</script" in src:
        sys.exit(f"{m} contains </script, which would end the page's script")
    parts.append(f"defs[{json.dumps(m)}] = function (module, exports, require) {{\n{src}\n}};\n")
parts.append("  return require;\n}\n")
bundle = "".join(parts)

drives = {}
for f in sorted((here / "drives" / "fitted").glob("*.json")):
    drives[f.stem] = json.loads(f.read_text())

page = (
    (here / "page.html").read_text(encoding="utf-8")
    .replace("/*BUNDLE*/", bundle)
    .replace("/*DRIVES*/", json.dumps(drives, separators=(",", ":")))
)

SITE_HEAD = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="description" content="Design the sound of a mechanical hard drive, or turn a recorded one into a model, synthesised live in your browser.">
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
print(f"wrote {out} ({len(page) // 1024} KB)")
