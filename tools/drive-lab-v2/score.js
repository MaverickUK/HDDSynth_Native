// Scores a drive against a recorded pack: how close is the synthesis?
//
//   node score.js SAMPLES_DIR [OUT_DIR] [--pack NAME]
//
// For each pack in SAMPLES_DIR: fit a drive (fit.js), render it (render.js) and
// measure both the same way. With OUT_DIR, also writes each fitted drive.json
// and A/B WAVs (recording, a short gap, then the synthesis) to listen to.
//
// The measures:
//   level   idle RMS, synthesis minus recording (dB)
//   bands   mean |difference| of the idle's third-octave bands (dB): the overall timbre
//   tones   share of the recording's strongest 12 tones (by power) that the synthesis
//           has within 0.5% in frequency and 3 dB in level
//   spinup / coast   the fitted times (s); "-" where the speed couldn't be tracked
const fs = require('fs'), path = require('path');
const dsp = require('./dsp.js');
const { fitPack, findTones } = require('./fit.js');
const { renderPack, renderIdle } = require('./render.js');
const { readWav, writeWav } = require('./wav.js');

function toneMatch(real, synth) {
  const top = real.slice().sort((a, b) => b.amp - a.amp).slice(0, 12);
  let total = 0, hit = 0;
  for (const t of top) {
    const p = t.amp * t.amp; total += p;
    if (synth.some((s) => Math.abs(s.hz - t.hz) < t.hz * 0.005 && Math.abs(20 * Math.log10(s.amp / t.amp)) < 3)) hit += p;
  }
  return total > 0 ? hit / total : 0;
}

function scorePack(dir, out) {
  const name = path.basename(dir);
  const { drive, fit } = fitPack(dir);
  const real = readWav(path.join(dir, 'idle.wav')), sr = real.sr;
  const synth = renderIdle(drive, sr, Math.max(4, real.x.length / sr));
  const a = dsp.thirdOctaveDb(real.x, sr), b = dsp.thirdOctaveDb(synth, sr);
  const bands = a.reduce((s, v, i) => s + Math.abs(v - b[i]), 0) / a.length;
  const r = {
    name, rpm: drive.spindle.rpm, tones: drive.tones.length,
    level: dsp.rmsDb(synth) - dsp.rmsDb(real.x), bands,
    toneMatch: toneMatch(findTones(real.x, sr).tones, findTones(synth, sr).tones),
    spinupS: fit.spinupS, coastS: fit.coastS,
  };
  if (out) {
    fs.mkdirSync(out, { recursive: true });
    fs.writeFileSync(path.join(out, name + '.json'), JSON.stringify(drive, null, 1) + '\n');
    const pack = renderPack(drive, sr), gap = new Float64Array(sr / 2);
    const cat = (...xs) => { const o = new Float64Array(xs.reduce((s, x) => s + x.length, 0)); let p = 0; for (const x of xs) { o.set(x, p); p += x.length; } return o; };
    const head = (x, s) => x.subarray(0, Math.min(x.length, Math.round(s * sr)));
    writeWav(path.join(out, name + '_idle_AB.wav'), cat(head(real.x, 3), gap, head(synth, 3)), sr);
    for (const k of ['spinup', 'spindown']) {
      const p = path.join(dir, k + '.wav');
      if (fs.existsSync(p)) writeWav(path.join(out, `${name}_${k}_AB.wav`), cat(readWav(p).x, gap, pack[k]), sr);
    }
  }
  return r;
}

if (require.main === module) {
  const a = process.argv.slice(2);
  const pi = a.indexOf('--pack'), only = pi >= 0 ? a.splice(pi, 2)[1] : null;
  const [root, out] = a;
  if (!root) { console.error('usage: node score.js SAMPLES_DIR [OUT_DIR] [--pack NAME]'); process.exit(2); }
  const packs = fs.readdirSync(root).filter((d) => fs.existsSync(path.join(root, d, 'idle.wav')) && (!only || d === only)).sort();
  const fmt = (v, d) => (v == null ? '-' : v.toFixed(d));
  console.log('pack                      rpm    tones  level dB  bands dB  tones matched  spinup s  coast s');
  for (const p of packs) {
    const r = scorePack(path.join(root, p), out);
    console.log(`${r.name.padEnd(24)} ${r.rpm.toFixed(0).padStart(6)} ${String(r.tones).padStart(6)} ${fmt(r.level, 1).padStart(9)} ${fmt(r.bands, 2).padStart(9)} ${(Math.round(r.toneMatch * 100) + '%').padStart(14)} ${fmt(r.spinupS, 1).padStart(9)} ${fmt(r.coastS, 1).padStart(8)}`);
  }
}

module.exports = { scorePack };
