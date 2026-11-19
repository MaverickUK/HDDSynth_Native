// Fits how a drive sounds as it slows, from its spindown.wav: the coast-down time,
// how the tones' and the noise's levels follow the speed, and the body curve.
//
// One idle recording can't separate a tone's own level from the body's gain at its
// frequency. The spindown can: every tone sweeps down through the body, lighting up
// as it crosses a resonance. The idle levels are held fixed throughout, so the body
// only reshapes the spindown, never the idle.
//
// Rendering audio for every candidate would be slow, but the synthesis is linear, so
// predict() works out the spindown's band-by-time spectrogram straight from the
// drive's parameters, and the search compares that with the recording's.
const dsp = require('./dsp.js');
const { makeEngine } = require('./engine.js');

const FRAME_S = 0.25;
const NOISE_HZ = makeEngine(16000).NOISE_HZ;
// The engine's coast law: ds/dt = -(s / tau + fric), stopping at exactly C seconds.
// k is friction's share of the deceleration at full speed (engine.js, coastConsts).
//   s(t) = (1 + a) e^(-t / tau) - a,  a = k / (1 - k),  tau = C / ln(1 / k)
function coastLaw(t, C, k) {
  if (t <= 0) return 1;
  if (k >= 0.999) return Math.max(0, 1 - t / C);
  const a = k / (1 - k), tau = C / Math.log(1 / k);
  return Math.max(0, (1 + a) * Math.exp(-t / tau) - a);
}
const FRICTIONS = [0.03, 0.082, 0.15, 0.25, 0.4, 0.6, 0.8, 0.999];
const TRACK_WEIGHT = 2000;   // dB^2 per (fraction of speed)^2: a 0.05 miss costs as much as 2.2 dB everywhere
const L10 = (p) => 10 * Math.log10(p + 1e-15);

// Bands perOct to the octave from 50 Hz (default thirds). Finer bands (twelfths) were
// tried for the fit, to make predicted tones land where the recording's are, but the
// search then dodged mismatched lines with too-short coast times: worse overall.
function bandEdges(sr, perOct) {
  perOct = perOct || 3;
  const h = Math.pow(2, 1 / (2 * perOct)), c = [];
  for (let f = 50; f * h < sr * 0.45; f *= Math.pow(2, 1 / perOct)) c.push(f);
  return { c, lo: c.map((f) => f / h), hi: c.map((f) => f * h) };
}

// The recording as third-octave band powers, one row per FRAME_S.
function bandFrames(x, sr, B) {
  const n = Math.round(FRAME_S * sr), rows = [];
  for (let s = 0; s + n <= x.length; s += n) {
    const { f, P, df } = dsp.welch(x.subarray(s, s + n), sr, 2048);
    rows.push(B.c.map((_, b) => dsp.bandPower(f, P, df, B.lo[b], B.hi[b])));
  }
  return rows;
}

// Piecewise-linear dB on a log-frequency axis (the engine's body curve).
function curveAt(hz, db, f) {
  if (f <= hz[0]) return db[0];
  for (let i = 1; i < hz.length; i++) if (f <= hz[i]) return db[i - 1] + (db[i] - db[i - 1]) * Math.log(f / hz[i - 1]) / Math.log(hz[i] / hz[i - 1]);
  return db[db.length - 1];
}

// m: { tones: [{f0, p0}], noise: [{fc0, p0}], fr0, follow, floor[b], nFrames, B }
// q: { C, t0, eT, eN, curveHz, curveDb }
function predict(m, q) {
  const { B } = m, nb = B.c.length, out = [];
  const lo0 = Math.log(B.lo[0]), step = Math.log(B.hi[0] / B.lo[0]);
  const bandOf = (f) => { const b = Math.floor((Math.log(f) - lo0) / step); return b >= 0 && b < nb ? b : -1; };
  const H = (f) => curveAt(q.curveHz, q.curveDb, f);
  for (let k = 0; k < m.nFrames; k++) {
    const row = m.floor.slice(), s = coastLaw((k + 0.5) * FRAME_S - q.t0, q.C, q.k);
    if (s > 0.001) {
      const sT = Math.pow(s, 2 * q.eT), sN = Math.pow(s, 2 * q.eN);
      for (const t of m.tones) {
        const f = t.f0 * s, b = bandOf(f);
        if (b >= 0) row[b] += t.p0 * sT * Math.pow(10, (H(f) - H(t.f0)) / 10);
      }
      const shift = Math.pow(Math.max(s, 0.05), m.follow);
      for (const n of m.noise) {
        const fc = n.fc0 * shift, lo = fc * Math.pow(2, -0.25), hi = fc * Math.pow(2, 0.25);
        const p = n.p0 * sN * Math.pow(10, (H(fc) - H(n.fc0)) / 10), span = Math.log(hi / lo);
        const b0 = Math.max(0, bandOf(lo) < 0 ? 0 : bandOf(lo)), b1 = bandOf(hi) < 0 ? nb - 1 : bandOf(hi);
        for (let b = b0; b <= b1; b++) {
          const ov = Math.log(Math.min(hi, B.hi[b]) / Math.max(lo, B.lo[b]));
          if (ov > 0) row[b] += p * ov / span;
        }
      }
    }
    out.push(row);
  }
  return out;
}

function error(m, real, q, smooth) {
  const pred = predict(m, q);
  let e = 0, n = 0;
  for (let k = 0; k < real.length; k++) for (let b = 0; b < real[k].length; b++) { const d = L10(real[k][b]) - L10(pred[k][b]); e += d * d; n++; }
  let pen = 0;
  for (let i = 1; i < q.curveDb.length; i++) pen += Math.pow(q.curveDb[i] - q.curveDb[i - 1], 2);
  let tr = 0;
  if (m.track) { for (const [t, s] of m.track) tr += Math.pow(coastLaw(t - q.t0, q.C, q.k) - s, 2); tr = TRACK_WEIGHT * tr / m.track.length; }
  return e / n + smooth * pen / q.curveDb.length + tr;
}

// drive: a fitted drive (flat body). noiseTargets: the idle's half-octave noise powers (dB).
// track: trustworthy tracked speed points [t, fraction of full speed], or null.
function fitSpindown(x, sr, drive, noiseTargets, track) {
  const B = bandEdges(sr, 3), real = bandFrames(x, sr, B);
  if (real.length < 6) return null;
  const fr0 = drive.spindle.rpm / 60, follow = drive.noise.follow == null ? 0.5 : drive.noise.follow;
  // The recording's own background: the quietest frame in each band.
  const floor = B.c.map((_, b) => Math.min(...real.map((r) => r[b])));
  const m = {
    B, fr0, follow, floor, nFrames: real.length, track,
    tones: drive.tones.map((t) => ({ f0: t.order * fr0, p0: Math.pow(10, t.db / 10) / 2 })),
    noise: NOISE_HZ.map((fc0, i) => ({ fc0, p0: Math.pow(10, noiseTargets[i] / 10) })).filter((n) => n.fc0 * Math.pow(2, 0.25) < sr * 0.45),
  };
  const curveHz = [];
  for (let f = 40; f < sr * 0.45; f *= Math.pow(2, 1 / 3)) curveHz.push(Math.round(f));
  const q = { C: 8, k: 0.082, t0: 0, eT: 2, eN: 2.5, curveHz, curveDb: curveHz.map(() => 0) };
  const SMOOTH = 0.05;

  // 1. Coast time and shape, start, and speed exponents, with a flat body.
  const len = real.length * FRAME_S;
  const before = error(m, real, { ...q, C: drive.spindle.coastS }, 0);
  let best = Infinity, bq = null;
  for (let C = Math.max(1, len * 0.4); C <= len + 4; C += 0.25)
    for (const k of FRICTIONS)
      for (const t0 of [-0.25, 0, 0.25, 0.5])
        for (let eT = 0.25; eT <= 2.5; eT += 0.25)
          for (let eN = 0.5; eN <= 3; eN += 0.5) {
            const e = error(m, real, { ...q, C, k, t0, eT, eN }, 0);
            if (e < best) { best = e; bq = { C, k, t0, eT, eN }; }
          }
  Object.assign(q, bq);

  // 2. The body curve, by coordinate descent in 1 dB steps (smoothed, within +-15 dB);
  //    then the speed parameters again around their values, and the curve once more.
  const descendCurve = (passes) => {
    let cur = error(m, real, q, SMOOTH);
    for (let pass = 0; pass < passes; pass++) {
      let moved = false;
      for (let i = 0; i < q.curveDb.length; i++) {
        for (const step of [1, -1]) {
          const old = q.curveDb[i];
          if (Math.abs(old + step) > 15) continue;
          q.curveDb[i] = old + step;
          const e = error(m, real, q, SMOOTH);
          if (e < cur - 1e-6) { cur = e; moved = true; } else q.curveDb[i] = old;
        }
      }
      if (!moved) break;
    }
  };
  descendCurve(20);
  for (const [k, step] of [['C', 0.25], ['eT', 0.125], ['eN', 0.25], ['t0', 0.05]]) {
    let cur = error(m, real, q, SMOOTH);
    for (let i = 0; i < 12; i++) {
      let moved = false;
      for (const d of [step, -step]) {
        const old = q[k]; q[k] = old + d;
        const e = q.C >= 1 && q.eT >= 0 && q.eN >= 0 ? error(m, real, q, SMOOTH) : Infinity;
        if (e < cur) { cur = e; moved = true; break; } q[k] = old;
      }
      if (!moved) break;
    }
  }
  descendCurve(20);

  // Only the curve's shape matters (the idle levels absorb any offset): centre it on 0 dB.
  const mean = q.curveDb.reduce((s, v) => s + v, 0) / q.curveDb.length;
  q.curveDb = q.curveDb.map((v) => v - mean);
  return { ...q, before: Math.sqrt(before), after: Math.sqrt(error(m, real, q, 0)) };
}

// Apply a fit to the drive, keeping every tone's and band's idle level as it was.
function applySpindown(drive, r) {
  const fr0 = drive.spindle.rpm / 60, H = (f) => curveAt(r.curveHz, r.curveDb, f);
  drive.spindle.coastS = Math.round(r.C * 100) / 100;
  drive.spindle.friction = r.k;
  drive.spindle.speedExp = r.eT;
  drive.noise.speedExp = r.eN;
  drive.bodyCurve = r.curveHz.map((hz, i) => ({ hz, db: r.curveDb[i] }));
  for (const t of drive.tones) t.db = Math.round((t.db - H(t.order * fr0)) * 10) / 10;
  drive.noise.db = drive.noise.db.map((v, i) => Math.round((v - H(NOISE_HZ[i])) * 10) / 10);
}

module.exports = { fitSpindown, applySpindown, bandEdges, bandFrames };
