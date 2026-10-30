# Drive Lab: the modelled drive's browser prototype

An interactive page for designing a modelled drive by ear: a top view of the
drive with its head, the drive designer (spindle, platters, bearing,
actuator, resonance, head events, enclosure), a mixer with one layer per
sound, workloads, spectrograms, and an A/B against a real pack's WAV. Its
JSON is the `drive.json` a pack carries (see the firmware README, "Modelled
drives").

Online at <https://maverickuk.github.io/HDDSynth_Native/drive-lab/>,
published by `.github/workflows/pages.yml` on every push to `main` that
changes this folder.

```sh
python3 tools/drive-lab/build.py              # -> drive-lab.html, the page body (for a claude.ai artifact)
python3 tools/drive-lab/build.py --site DIR   # -> DIR/index.html, the website (adds drive.json download)
```

- `engine.js` is the drive model and synthesis. The firmware's
  `src/drive_engine.*` is a port of it: **change both together.**
  `compare.js` (run by `test/run_native_tests.sh` when node is installed)
  renders the same scenario on both and fails if they disagree.
- `page.html` is the page around it; `build.py` inlines the engine.
