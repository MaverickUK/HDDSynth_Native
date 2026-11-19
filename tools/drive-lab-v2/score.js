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
const { renderPack, renderIdle, renderAccess } = require('./render.js');
const { readWav, writeWav } = require('./wav.js');
const { bandEdges, bandFrames } = require('./spindown_fit.js');
const { measure, reference, overIdle, excessError } = require('./seek_fit.js');
const { compareEvents } = require('./head_fit.js');

function toneMatch(real, synth) {
  const top = real.slice().sort((a, b) => b.amp - a.amp).slice(0, 12);
  let total = 0, hit = 0;
  for (const t of top) {
    const p = t.amp * t.amp; total += p;
    if (synth.some((s) => Math.abs(s.hz - t.hz) < t.hz * 0.005 && Math.abs(20 * Math.log10(s.amp / t.amp)) < 3)) hit += p;
  }
  return total > 0 ? hit / total : 0;
}

// The spindown: mean |difference| (dB) of third-octave bands, frame by frame over the
// recording's length. Both sides are floored at the recording's quietest level in each
// band, so its background noise doesn't count.
function spindownError(real, synth, sr) {
  const B = bandEdges(sr), a = bandFrames(real, sr, B), b = bandFrames(synth, sr, B);
  const floor = B.c.map((_, i) => Math.min(...a.map((r) => r[i])));
  const L = (p, i) => 10 * Math.log10(Math.max(p, floor[i]) + 1e-15);
  let e = 0, n = 0;
  a.forEach((row, k) => row.forEach((p, i) => { e += Math.abs(L(p, i) - L(b[k] ? b[k][i] : 0, i)); n++; }));
  return e / n;
}

function scorePack(dir, out, opts) {
  const name = path.basename(dir);
  const { drive, fit } = fitPack(dir, opts);
  const real = readWav(path.join(dir, 'idle.wav')), sr = real.sr;
  const synth = renderIdle(drive, sr, Math.max(4, real.x.length / sr));
  const a = dsp.thirdOctaveDb(real.x, sr), b = dsp.thirdOctaveDb(synth, sr);
  const bands = a.reduce((s, v, i) => s + Math.abs(v - b[i]), 0) / a.length;
  const r = {
    name, rpm: drive.spindle.rpm, tones: drive.tones.length,
    level: dsp.rmsDb(synth) - dsp.rmsDb(real.x), bands,
    toneMatch: toneMatch(findTones(real.x, sr).tones, findTones(synth, sr).tones),
    spinupS: fit.spinupS, coastS: drive.spindle.coastS, spindown: null,
  };
  const pack = renderPack(drive, sr);
  const sdPath = path.join(dir, 'spindown.wav');
  if (fs.existsSync(sdPath)) r.spindown = spindownError(readWav(sdPath).x, pack.spindown, sr);
  // The head events: found the same way in the recording and the render.
  r.heads = {};
  for (const k of ['spinup', 'spindown']) {
    const p = path.join(dir, k + '.wav');
    if (fs.existsSync(p) && drive.heads) r.heads[k] = compareEvents(readWav(p).x, pack[k], sr);
  }
  // The seeks, as heard: access.wav over the idle loop against the synthesis while busy.
  // Both are detected against the recording's idle (what stands out over that is a
  // seek), and each one's added spectrum is measured over its own idle.
  const acPath = path.join(dir, 'access.wav');
  let realAccess = null, synthAccess = null;
  if (fs.existsSync(acPath) && drive.actuator) {
    realAccess = overIdle(readWav(acPath).x, real.x);
    synthAccess = renderAccess(drive, sr, realAccess.length / sr);
    const refReal = reference(real.x, sr);
    const a = measure(realAccess, sr, refReal), b = measure(synthAccess, sr, { ...reference(synth, sr), thr: refReal.thr });
    r.seeks = { rate: [a.rate, b.rate], busy: [a.busy, b.busy], err: excessError(a, b, sr), level: b.level - a.level };
  }
  if (out) {
    fs.mkdirSync(out, { recursive: true });
    fs.writeFileSync(path.join(out, name + '.json'), JSON.stringify(drive, null, 1) + '\n');
    const gap = new Float64Array(sr / 2);
    const cat = (...xs) => { const o = new Float64Array(xs.reduce((s, x) => s + x.length, 0)); let p = 0; for (const x of xs) { o.set(x, p); p += x.length; } return o; };
    const head = (x, s) => x.subarray(0, Math.min(x.length, Math.round(s * sr)));
    writeWav(path.join(out, name + '_idle_AB.wav'), cat(head(real.x, 3), gap, head(synth, 3)), sr);
    for (const k of ['spinup', 'spindown']) {
      const p = path.join(dir, k + '.wav');
      if (fs.existsSync(p)) writeWav(path.join(out, `${name}_${k}_AB.wav`), cat(readWav(p).x, gap, pack[k]), sr);
    }
    if (realAccess) writeWav(path.join(out, `${name}_access_AB.wav`), cat(head(realAccess, 5), gap, head(synthAccess, 5)), sr);
  }
  return r;
}

if (require.main === module) {
  const a = process.argv.slice(2);
  const pi = a.indexOf('--pack'), only = pi >= 0 ? a.splice(pi, 2)[1] : null;
  const ni = a.indexOf('--no-spindown-fit'), noSpindown = ni >= 0; if (noSpindown) a.splice(ni, 1);
  const [root, out] = a;
  if (!root) { console.error('usage: node score.js SAMPLES_DIR [OUT_DIR] [--pack NAME] [--no-spindown-fit]'); process.exit(2); }
  const packs = fs.readdirSync(root).filter((d) => fs.existsSync(path.join(root, d, 'idle.wav')) && (!only || d === only)).sort();
  const fmt = (v, d) => (v == null ? '-' : v.toFixed(d));
  console.log('pack                      rpm    tones  level dB  bands dB  tones matched  spinup s  coast s  spindown dB  seeks/s real/synth  busy real/synth  seek level dB  seek spectrum dB  head events: spinup matched/real (synth)  spindown  level dB');
  for (const p of packs) {
    const r = scorePack(path.join(root, p), out, { noSpindown });
    const s = r.seeks, h = r.heads;
    const hc = (c) => (c ? `${c.matched}/${c.real} (${c.synth})` : '-');
    const hl = ['spinup', 'spindown'].map((k) => h[k] && h[k].levelErr != null ? [h[k].levelErr, h[k].matched] : null).filter(Boolean);
    const hErr = hl.length ? hl.reduce((a, [e, n]) => a + e * n, 0) / hl.reduce((a, [, n]) => a + n, 0) : null;
    console.log(`${r.name.padEnd(24)} ${r.rpm.toFixed(0).padStart(6)} ${String(r.tones).padStart(6)} ${fmt(r.level, 1).padStart(9)} ${fmt(r.bands, 2).padStart(9)} ${(Math.round(r.toneMatch * 100) + '%').padStart(14)} ${fmt(r.spinupS, 1).padStart(9)} ${fmt(r.coastS, 1).padStart(8)} ${fmt(r.spindown, 1).padStart(12)}` +
      (s ? `  ${(s.rate[0].toFixed(1) + ' / ' + s.rate[1].toFixed(1)).padStart(18)}  ${(Math.round(s.busy[0] * 100) + '% / ' + Math.round(s.busy[1] * 100) + '%').padStart(15)}  ${fmt(s.level, 1).padStart(13)}  ${fmt(s.err, 1).padStart(16)}` : '  no seeks'.padEnd(70)) +
      `  ${hc(h.spinup).padStart(39)}  ${hc(h.spindown).padStart(8)}  ${fmt(hErr, 1).padStart(8)}`);
  }
}

module.exports = { scorePack };
