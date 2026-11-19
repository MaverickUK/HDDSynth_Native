// Recorded seek sounds ("grains"): an optional extra for a drive, never required.
//
//   node grains.js PACK_DIR drive.json [--max N] [--mix M]
//
// Cuts the seeks out of the pack's access.wav, writes them side by side to
// <drive>.grains.wav next to drive.json, and lists them in drive.json's `grains`:
//   grains: { file, mix, items: [{ start, len, ms }] }
// While seeking, the engine plays the grain nearest the seek's length, mixed with
// the synthesised seek: mix 0 is pure synthesis, 1 only the recording.
//
// access.wav is quiet between seeks (the player mixes it over the idle loop), so a
// grain cut from it is the seek's own sound, close enough without the drive running
// under it.
const fs = require('fs'), path = require('path');
const { readWav, writeWav } = require('./wav.js');
const { reference, detect, HOP_S } = require('./seek_fit.js');

const PRE_MS = 3, POST_MS = 8, FADE_MS = 2, MAX_MS = 150;

function extractGrains(access, idle, sr, opts) {
  opts = opts || {};
  const max = opts.max || 24;
  // Find the seeks as heard (access over the idle), cut them from access.wav alone.
  const mix = new Float64Array(access.length);
  for (let i = 0; i < mix.length; i++) mix[i] = access[i] + idle[i % idle.length];
  const { ev } = detect(mix, sr, reference(idle, sr).thr);
  const hop = Math.round(sr * HOP_S), pre = Math.round(PRE_MS / 1000 * sr), post = Math.round(POST_MS / 1000 * sr);
  const cands = ev.map(([a, b]) => ({ s: a * hop - pre, e: b * hop + post, ms: (b - a) * HOP_S * 1000 }))
    .filter((g) => g.s >= 0 && g.e <= access.length && g.ms <= MAX_MS);
  if (!cands.length) return null;
  // Spread the picks over the range of lengths: evenly spaced through them, shortest to longest.
  cands.sort((p, q) => p.ms - q.ms);
  const picks = [];
  const n = Math.min(max, cands.length);
  for (let i = 0; i < n; i++) picks.push(cands[Math.round(i * (cands.length - 1) / Math.max(1, n - 1))]);
  const fade = Math.round(FADE_MS / 1000 * sr);
  const total = picks.reduce((s, g) => s + g.e - g.s, 0);
  const data = new Float64Array(total), items = [];
  let at = 0;
  for (const g of picks) {
    const len = g.e - g.s;
    for (let i = 0; i < len; i++) {
      const w = Math.min(1, i / fade, (len - 1 - i) / fade);      // short fades: no clicks at the cuts
      data[at + i] = access[g.s + i] * (0.5 - 0.5 * Math.cos(Math.PI * Math.max(0, w)));
    }
    items.push({ start: at, len, ms: Math.round(g.ms) });
    at += len;
  }
  return { data, items };
}

if (require.main === module) {
  const a = process.argv.slice(2);
  const opt = (k) => { const i = a.indexOf(k); return i >= 0 ? a.splice(i, 2)[1] : undefined; };
  const max = Number(opt('--max')) || 24, mixArg = opt('--mix');
  const [dir, file] = a;
  if (!dir || !file) { console.error('usage: node grains.js PACK_DIR drive.json [--max N] [--mix M]'); process.exit(2); }
  const acc = readWav(path.join(dir, 'access.wav')), idle = readWav(path.join(dir, 'idle.wav'));
  const g = extractGrains(acc.x, idle.x, acc.sr, { max });
  if (!g) { console.error('no seeks found in', dir); process.exit(1); }
  const drive = JSON.parse(fs.readFileSync(file, 'utf8'));
  const wavName = path.basename(file).replace(/\.json$/, '') + '.grains.wav';
  writeWav(path.join(path.dirname(file), wavName), g.data, acc.sr);
  drive.grains = { file: wavName, mix: mixArg != null ? Number(mixArg) : (drive.grains && drive.grains.mix) || 1, items: g.items };
  fs.writeFileSync(file, JSON.stringify(drive, null, 1) + '\n');
  console.log(`${g.items.length} grains (${g.items[0].ms}-${g.items[g.items.length - 1].ms} ms, ${(g.data.length * 2 / 1024).toFixed(0)} KB at 16-bit) -> ${wavName}`);
}

module.exports = { extractGrains };
