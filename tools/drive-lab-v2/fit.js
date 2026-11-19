// Fits a drive's spindle from a recorded pack: a starting point to edit in the Lab,
// never a requirement (a drive can be designed from scratch).
//
//   node fit.js PACK_DIR [--rpm N] [-o drive.json]
//
// From idle.wav:     the rotation rate, the tones (as orders of it) and the air-noise bands.
// From spinup.wav and spindown.wav (if present): spin-up and coast-down times, by
//                    tracking the rotation rate through them.
const fs = require('fs'), path = require('path');
const dsp = require('./dsp.js');
const { makeEngine } = require('./engine.js');
const { renderIdle } = require('./render.js');
const { readWav } = require('./wav.js');

const MAX_TONES = 40, PEAK_DB = 6, FLOOR_HZ = 75;
const NOISE_HZ = makeEngine(16000).NOISE_HZ;
const db = (p) => 10 * Math.log10(p + 1e-20);

// Tones: spectral peaks standing PEAK_DB above a median-smoothed floor.
function findTones(x, sr) {
  const { f, P, df } = dsp.welch(x, sr, 32768, 'bh');
  const L = Array.from(P, db);
  const floorL = dsp.medfilt(L, (Math.round(FLOOR_HZ / df) | 1));
  const floorP = floorL.map((v) => Math.pow(10, v / 10));
  const rel = L.map((v, i) => v - floorL[i]);
  const peaks = dsp.findPeaks(rel, PEAK_DB, 4).filter((i) => f[i] > 15 && f[i] < sr * 0.45).slice(0, MAX_TONES);
  const tones = peaks.map((i) => {
    // Parabolic interpolation on the log spectrum for the exact frequency.
    const a = L[i - 1], b = L[i], c = L[i + 1], den = a - 2 * b + c;
    const hz = (i + (den !== 0 ? 0.5 * (a - c) / den : 0)) * df;
    // The tone's power: what it adds above the floor across its window's main lobe.
    let pw = 0;
    for (let k = Math.max(0, i - 6); k <= Math.min(P.length - 1, i + 6); k++) pw += Math.max(0, P[k] - floorP[k]);
    return { hz, amp: Math.sqrt(2 * pw * df) };
  });
  return { tones, f, floorP, df };
}

// The rotation rate: the low peak whose multiples explain the most tone energy.
// A sub-multiple explains everything its multiple does, so take the highest
// candidate within 85% of the best.
function findRotation(tones) {
  const cands = tones.filter((t) => t.hz >= 20 && t.hz <= 250).map((t) => t.hz);
  const explain = (fr) => {
    let s = 0;
    for (const t of tones) { const n = Math.round(t.hz / fr); if (n >= 1 && Math.abs(t.hz / fr - n) < 0.02) s += t.amp; }
    return s;
  };
  if (!cands.length) return null;
  const sc = cands.map(explain), best = Math.max(...sc);
  let fr = 0;
  cands.forEach((c, i) => { if (sc[i] >= 0.85 * best && c > fr) fr = c; });
  // Refine by least squares over the tones it explains: high orders pin it down.
  let num = 0, den = 0;
  for (const t of tones) {
    const n = Math.round(t.hz / fr);
    if (n >= 1 && Math.abs(t.hz / fr - n) < 0.02) { num += t.amp * n * t.hz; den += t.amp * n * n; }
  }
  return den > 0 ? num / den : fr;
}

function toOrders(tones, fr) {
  return tones.map((t) => {
    let order = t.hz / fr;
    order = Math.abs(order - Math.round(order)) < 0.01 ? Math.round(order) : Math.round(order * 1000) / 1000;
    return { order, db: Math.round(20 * Math.log10(t.amp) * 10) / 10 };
  }).sort((a, b) => a.order - b.order);
}

// Air noise: the floor's power in each of the engine's half-octave bands. Bands
// above what the recording holds fall away at 6 dB per band.
function noiseTargets(f, floorP, df, sr) {
  const out = [];
  for (const c of NOISE_HZ) {
    const hi = c * Math.pow(2, 0.25);
    if (hi < sr * 0.45) out.push(db(dsp.bandPower(f, floorP, df, c * Math.pow(2, -0.25), hi)));
    else out.push(out.length ? out[out.length - 1] - 6 : -90);
  }
  return out;
}

// The engine's bands overlap, so set them by measuring what the engine produces
// and correcting: three rounds.
function calibrateNoise(drive, target, sr) {
  const probe = JSON.parse(JSON.stringify(drive));
  probe.tones = []; probe.noise.db = target.slice();
  for (let round = 0; round < 3; round++) {
    const y = renderIdle(probe, sr, 4);
    const { f, P, df } = dsp.welch(y, sr, 8192);
    NOISE_HZ.forEach((c, i) => {
      const hi = c * Math.pow(2, 0.25);
      if (hi >= sr * 0.45) return;
      const got = db(dsp.bandPower(f, P, df, c * Math.pow(2, -0.25), hi));
      probe.noise.db[i] += Math.max(-20, Math.min(20, target[i] - got));
    });
  }
  return probe.noise.db.map((v) => Math.round(v * 10) / 10);
}

// The rotation rate through a spin-up or spindown: each short frame is scored
// against the drive's comb of tones at every candidate rate, and a Viterbi pass
// picks the smoothest high-scoring path (the speed can't jump).
// dir: +1 for a spin-up, -1 for a spindown, 0 for no constraint.
function trackSpeed(x, sr, orders, fr0, dir) {
  // Only whole orders: fractional ones are often fixed-frequency sounds (electrical,
  // the room) that don't move with the speed and would pin the track at full speed.
  orders = orders.filter((o) => Number.isInteger(o.order));
  const n =1 << Math.round(Math.log2(sr * 0.25)), hop = Math.round(sr * 0.05);
  const { S, t, df } = dsp.frames(x, sr, n, hop, 'hann');
  const G = 300, lo = 0.03 * fr0, hi = 1.08 * fr0, grid = [];
  for (let g = 0; g < G; g++) grid.push(lo * Math.pow(hi / lo, g / (G - 1)));
  const w = orders.map((o) => Math.pow(10, o.db / 40));
  const W = w.reduce((s, v) => s + v, 0);
  const score = S.map((P) => {
    const L = Array.from(P, db), med = dsp.medfilt(L, 31);
    return grid.map((fr) => {
      let s = 0;
      orders.forEach((o, i) => {
        // Below 60 Hz the spectrum's steep rumble would score any comb highly.
        const hz = o.order * fr, k = Math.round(hz / df);
        if (hz >= 60 && hz < sr * 0.45) s += w[i] * Math.max(0, L[k] - med[k]);
      });
      return s / W;                     // over every tone: a comb with few tones in range scores low
    });
  });
  const STEP = 8, PEN = 0.3;
  let cost = score[0].map((v) => -v);
  const back = [];
  for (let k = 1; k < score.length; k++) {
    const nc = new Array(G), bk = new Array(G);
    for (let g = 0; g < G; g++) {
      let bc = Infinity, bi = g;
      // A spin-up only speeds up and a spindown only slows (one step of slack for noise).
      const from = dir > 0 ? g - STEP : dir < 0 ? g - 1 : g - STEP;
      const to = dir > 0 ? g + 1 : dir < 0 ? g + STEP : g + STEP;
      for (let h = Math.max(0, from); h <= Math.min(G - 1, to); h++) {
        const c = cost[h] + PEN * Math.abs(g - h);
        if (c < bc) { bc = c; bi = h; }
      }
      nc[g] = bc - score[k][g]; bk[g] = bi;
    }
    cost = nc; back.push(bk);
  }
  let g = cost.indexOf(Math.min(...cost));
  const path = [g];
  for (let k = back.length - 1; k >= 0; k--) { g = back[k][g]; path.unshift(g); }
  return { t, fr: path.map((i) => grid[i]) };
}

// The engine's speed laws (fraction of full speed, t seconds after power on/off):
//   spin-up  s(t) = 2.5 (1 - e^(-t / 2S)), until it reaches 1
//   coast    s(t) = (1 + ct) e^(-t / tau) - ct, tau = C / 2.5, ct = 0.0894 (windage plus friction)
const spinLaw = (t, S) => (t <= 0 ? 0 : Math.min(1, 2.5 * (1 - Math.exp(-t / (2 * S)))));
const coastLaw = (t, C) => (t <= 0 ? 1 : Math.max(0, 1.0894 * Math.exp(-t / (C / 2.5)) - 0.0894));

// Spin-up time S or coast time C: a least-squares fit of the law to the tracked
// speed, also solving for when the power changed. Only the mid range is used: at
// low speed the tones are faint and the track is unreliable.
function fitTimes(spin, fr0, rising) {
  const pts = [];
  spin.fr.forEach((v, i) => { const s = v / fr0; if (s >= 0.4 && s <= 0.95) pts.push([spin.t[i], s]); });
  if (pts.length < 6) return null;
  const law = rising ? spinLaw : coastLaw;
  let best = Infinity, bestT = null;
  for (let T = 1; T <= 40; T += 0.1) {
    for (let t0 = -2; t0 <= 6; t0 += 0.05) {
      let e = 0;
      for (const [t, s] of pts) { const d = law(t - t0, T) - s; e += d * d; }
      if (e < best) { best = e; bestT = T; }
    }
  }
  // A poor fit means the track wandered, and one at the search's limit means it sat
  // still (locked onto a false comb): trust neither.
  if (bestT <= 1.05 || bestT >= 39.9) return null;
  return Math.sqrt(best / pts.length) < 0.08 ? bestT : null;
}

function fitPack(dir, opts) {
  opts = opts || {};
  const name = path.basename(path.resolve(dir));
  const idle = readWav(path.join(dir, 'idle.wav')), sr = idle.sr;
  const { tones, f, floorP, df } = findTones(idle.x, sr);
  const fr = opts.rpm ? opts.rpm / 60 : findRotation(tones);
  if (!fr) throw new Error(name + ': no rotation rate found; pass --rpm');
  const orders = toOrders(tones, fr);
  const drive = {
    version: 3,
    name,
    note: 'Fitted from ' + name + ' by fit.js',
    spindle: { rpm: Math.round(fr * 600) / 10, spinupS: 5, coastS: 8, speedExp: 2, motorBoostDb: 0, jitter: 0.0005, wobble: 0 },
    tones: orders,
    body: [],
    noise: { db: [], speedExp: 2.5, follow: 0.5 },
    master: 1,
  };
  drive.noise.db = calibrateNoise(drive, noiseTargets(f, floorP, df, sr), sr);
  const fit = {};
  for (const [file, rising, key, lo, hi] of [['spinup.wav', true, 'spinupS', 1, 30], ['spindown.wav', false, 'coastS', 2, 40]]) {
    const p = path.join(dir, file);
    if (!fs.existsSync(p)) continue;
    const w = readWav(p);
    const v = fitTimes(trackSpeed(w.x, sr, orders, fr, rising ? 1 : -1), fr, rising);
    fit[key] = v;
    if (v) drive.spindle[key] = Math.round(Math.max(lo, Math.min(hi, v)) * 10) / 10;
  }
  return { drive, fit };
}

if (require.main === module) {
  const a = process.argv.slice(2);
  const dir = a.find((s, i) => !s.startsWith('-') && !['--rpm', '-o'].includes(a[i - 1]));
  if (!dir) { console.error('usage: node fit.js PACK_DIR [--rpm N] [-o drive.json]'); process.exit(2); }
  const opt = (k) => { const i = a.indexOf(k); return i >= 0 ? a[i + 1] : undefined; };
  const { drive, fit } = fitPack(dir, { rpm: Number(opt('--rpm')) || 0 });
  const json = JSON.stringify(drive, null, 1);
  if (opt('-o')) fs.writeFileSync(opt('-o'), json + '\n'); else console.log(json);
  console.error(`${drive.name}: ${drive.spindle.rpm} rpm, ${drive.tones.length} tones, spin-up ${fit.spinupS ? fit.spinupS.toFixed(1) + ' s' : 'not tracked'}, coast ${fit.coastS ? fit.coastS.toFixed(1) + ' s' : 'not tracked'}`);
}

module.exports = { fitPack, findTones, findRotation, trackSpeed };
