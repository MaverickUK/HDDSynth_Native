// Fits a drive's actuator (its seeks) and workload from a recorded pack's access.wav.
//
// A seek shows up as a short burst over the idle. measure() finds them the same way
// in any recording, against the drive's idle: how many, how long, the gaps between
// them, and their spectrum (what they add to the idle). The fit then renders the
// drive's seeks, adds them to the recorded idle, measures that the same way, and
// corrects the actuator until the two measure alike.
const dsp = require('./dsp.js');
const { renderAccess } = require('./render.js');

// Three bands, so a seek counts whether it's a low rock (most drives) or high ringing
// (Mark_Original, the WD Caviar).
const BANDS = [[120, 800], [800, 2500], [2500, 7000]];
const HOP_S = 0.002, ON_DB = 6, MERGE_HOPS = 3;
const L10 = (p) => 10 * Math.log10(p + 1e-20);
const pct = (a, p) => { if (!a.length) return 0; const s = a.slice().sort((x, y) => x - y); return s[Math.min(s.length - 1, Math.floor(p * s.length))]; };

function bandRows(x, sr) {
  const n = 1 << Math.round(Math.log2(sr * 0.008)), hop = Math.round(sr * HOP_S);
  const { S, df } = dsp.frames(x, sr, n, hop, 'hann');
  const f = S.length ? Array.from({ length: S[0].length }, (_, k) => k * df) : [];
  return S.map((P) => BANDS.map(([lo, hi]) => dsp.bandPower(f, P, df, lo, Math.min(hi, sr * 0.45))));
}

// Each band's threshold: the idle's 99th percentile, plus ON_DB.
function thresholds(idle, sr) {
  const rows = bandRows(idle, sr);
  return BANDS.map((_, b) => pct(rows.map((r) => r[b]), 0.99) * Math.pow(10, ON_DB / 10));
}

function detect(x, sr, thr) {
  const rows = bandRows(x, sr);
  const on = rows.map((r) => r.some((p, b) => p > thr[b]));
  const ev = [];
  for (let i = 0; i < on.length;) {
    if (!on[i]) { i++; continue; }
    let j = i, gap = 0;
    while (j < on.length && gap < MERGE_HOPS) { gap = on[j] ? 0 : gap + 1; j++; }
    const e = j - gap;
    if (e - i >= 2) ev.push([i, e]);
    i = j;
  }
  return { on, ev };
}

// The seeks in x (an access recording, or a render of one), measured against the idle.
// idleRef: { thr, P } from reference(idle).
function reference(idle, sr) {
  const { P, f, df } = dsp.welch(idle, sr, 512);
  return { thr: thresholds(idle, sr), P, f, df };
}
function measure(x, sr, ref) {
  const { on, ev } = detect(x, sr, ref.thr);
  const ms = HOP_S * 1000, secs = x.length / sr;
  const durs = ev.map(([a, b]) => (b - a) * ms);
  const gaps = ev.slice(1).map(([a], i) => (a - ev[i][1]) * ms);
  // What the seeks add: the mean spectrum of the frames inside them, less the idle's.
  const hop = Math.round(sr * HOP_S), n = 512;
  const fr = dsp.frames(x, sr, n, 128);
  const P = new Float64Array(n / 2 + 1);
  let cnt = 0;
  fr.S.forEach((F, k) => {
    const c = Math.floor((k * 128 + n / 2) / hop);
    if (on[c]) { for (let i = 0; i < P.length; i++) P[i] += F[i]; cnt++; }
  });
  const excess = Array.from(P, (p, i) => Math.max(0, (cnt ? p / cnt : 0) - ref.P[i]));
  // Per seek: its level (dB) and crest factor (peak over RMS, dB). How the level grows
  // with the seek's length is the slope of level against log2(duration).
  const lv = [], ld = [], crest = [];
  for (const [a, b] of ev) {
    let s = 0, pk = 0;
    for (let i = a * hop; i < b * hop && i < x.length; i++) { s += x[i] * x[i]; pk = Math.max(pk, Math.abs(x[i])); }
    const rms = Math.sqrt(s / ((b - a) * hop));
    lv.push(20 * Math.log10(rms + 1e-9)); ld.push(Math.log2((b - a) * ms)); crest.push(20 * Math.log10(pk / (rms + 1e-9)));
  }
  const mean = (a) => a.reduce((s, v) => s + v, 0) / Math.max(1, a.length);
  const mx = mean(ld), my = mean(lv);
  let sxy = 0, sxx = 0;
  ld.forEach((v, i) => { sxy += (v - mx) * (lv[i] - my); sxx += (v - mx) * (v - mx); });
  return {
    n: ev.length, rate: ev.length / secs, busy: on.filter(Boolean).length / Math.max(1, on.length),
    slope: sxx > 0 ? sxy / sxx : 0, crest: mean(crest), level: my,
    dur: [0.1, 0.5, 0.9].map((p) => pct(durs, p)), gap: [0.1, 0.5, 0.9].map((p) => pct(gaps, p)),
    excess, df: sr / n,
  };
}

// The excess spectrum on a log-frequency grid (dB), lightly smoothed.
const GRID_PER_OCT = 24;
function logGrid(m, sr) {
  const hz = [], v = [];
  for (let f = 80; f < sr * 0.45; f *= Math.pow(2, 1 / GRID_PER_OCT)) {
    const k = f / m.df, k0 = Math.floor(k), u = k - k0;
    hz.push(f); v.push((m.excess[k0] || 0) * (1 - u) + (m.excess[k0 + 1] || 0) * u);
  }
  const sm = v.map((_, i) => { let s = 0, c = 0; for (let j = i - 2; j <= i + 2; j++) if (j >= 0 && j < v.length) { s += v[j]; c++; } return s / c; });
  return { hz, db: sm.map(L10) };
}
// Level (dB) of the excess around f: a sixth of an octave either side.
function excessAt(m, f) {
  let s = 0;
  for (let k = 0; k < m.excess.length; k++) { const fk = k * m.df; if (fk >= f / 1.122 && fk < f * 1.122) s += m.excess[k]; }
  return L10(s * m.df);
}

// The arm's modes: the excess spectrum's peaks, up to 6, within 25 dB of the strongest.
function findModes(m, sr) {
  const g = logGrid(m, sr);
  const top = Math.max(...g.db);
  const peaks = dsp.findPeaks(g.db.map((v) => v - (top - 25)), 0, GRID_PER_OCT / 3).slice(0, 6);
  return peaks.map((i) => {
    // Q from the -3 dB width (the grid is coarse, so keep it within reason).
    let a = i, b = i;
    while (a > 0 && g.db[a] > g.db[i] - 3) a--;
    while (b < g.db.length - 1 && g.db[b] > g.db[i] - 3) b++;
    const q = g.hz[i] / Math.max(1, g.hz[b] - g.hz[a]);
    return { hz: Math.round(g.hz[i]), q: Math.round(Math.min(12, Math.max(1.5, q)) * 10) / 10, db: 0 };
  }).sort((p, q) => p.hz - q.hz);
}

// A stepper's seeks all hum at its step rate, whatever their length; a voice coil's
// low rock wanders more from seek to seek. Returns the step rate (Hz), or 0.
function stepRate(x, sr, ref) {
  const { ev } = detect(x, sr, ref.thr), hop = Math.round(sr * HOP_S), f0s = [];
  for (const [a, b] of ev) {
    if (b - a < 4) continue;                       // at least 8 ms
    const N = 8192, re = new Float64Array(N), im = new Float64Array(N), len = Math.min(N, (b - a) * hop);
    for (let i = 0; i < len; i++) re[i] = x[a * hop + i] * (0.5 - 0.5 * Math.cos(2 * Math.PI * i / len));
    dsp.fft(re, im);
    let best = 0, bk = 0;
    for (let k = Math.ceil(120 * N / sr); k < 800 * N / sr; k++) { const p = re[k] * re[k] + im[k] * im[k]; if (p > best) { best = p; bk = k; } }
    f0s.push(bk * sr / N);
  }
  if (f0s.length < 10) return 0;
  const p50 = pct(f0s, 0.5);
  return (pct(f0s, 0.9) - pct(f0s, 0.1)) / p50 < 0.06 ? p50 : 0;
}

// Add the rendered seeks to the recorded idle (looped to length).
function overIdle(seeks, idle) {
  const y = new Float64Array(seeks.length);
  for (let i = 0; i < y.length; i++) y[i] = seeks[i] + idle[i % idle.length];
  return y;
}

// x: access.wav, idle: idle.wav. Returns { actuator, workload, real, synth } or null.
// The player mixes access.wav over the idle loop (at equal levels by default: access.wav
// itself is quiet between seeks), so that mix is what the seeks are fitted to.
function fitSeeks(x, idle, sr, drive, opts) {
  opts = opts || {};
  x = overIdle(x, idle);
  const ref = reference(idle, sr), real = measure(x, sr, ref);
  if (real.n < 5) return null;
  const rev = 60 / drive.spindle.rpm, secs = x.length / sr;
  const stepHz = opts.stepper === false ? 0 : stepRate(x, sr, ref);
  const stepper = !!stepHz;
  const act = {
    kind: stepper ? 'stepper' : 'voice',
    trackMs: Math.max(1, real.dur[0] * 0.5), fullStrokeMs: Math.max(4, real.dur[2]),
    // The 90th percentile of the distance between two random points is 0.68 of the stroke.
    stepHz: Math.round(stepHz || 250), strokeSteps: Math.max(2, Math.round(real.dur[2] / 1000 * (stepHz || 250) / 0.68)),
    settleMs: 3, levelExp: 0.5, thumpDb: 0, buzzDb: -6, clickDb: -6, db: 0,
    modes: findModes(real, sr),
  };
  if (!act.modes.length) return null;
  // A very regular rhythm is sequential reading: a short step every few revolutions.
  const seq = real.gap[2] < 1.6 * Math.max(1, real.gap[0]) && real.rate > 25;
  const work = seq
    ? { gapMs: 0, locality: 0, seqShare: 1, seqRevs: Math.max(1, Math.round((real.gap[1] + real.dur[1]) / 1000 / rev)) }
    : { gapMs: Math.max(0.5, real.gap[1] - rev * 500), locality: 0.2, seqShare: 0 };
  const d = { ...drive, actuator: act, workload: work };
  let synth = null;
  for (let round = 0; round < 6; round++) {
    synth = measure(overIdle(renderAccess(d, sr, secs, { seeksOnly: true }), idle), sr, ref);
    if (synth.n === 0) { act.modes.forEach((m) => { m.db += 10; }); continue; }
    for (const m of act.modes) m.db += Math.max(-15, Math.min(15, excessAt(real, m.hz) - excessAt(synth, m.hz))) * 0.8;
    if (!seq && round < 5) {
      // The detected durations include the ringing, so scale the times by what came out.
      if (stepper) act.strokeSteps = Math.max(2, Math.round(act.strokeSteps * Math.pow((real.dur[2] + 1) / (synth.dur[2] + 1), 0.8)));
      else {
        act.trackMs *= Math.pow((real.dur[0] + 1) / (synth.dur[0] + 1), 0.7);
        act.fullStrokeMs *= Math.pow((real.dur[2] + 1) / (synth.dur[2] + 1), 0.7);
        act.trackMs = Math.max(0.5, Math.min(act.trackMs, act.fullStrokeMs * 0.8));
      }
      // How much louder long seeks are than short ones, and how spiky they are: fewer
      // clicks against more buzz smooths them.
      act.levelExp = Math.max(0, Math.min(1.5, act.levelExp + 0.6 * (real.slope - synth.slope) / 6));
      const dc = Math.max(-6, Math.min(6, real.crest - synth.crest));
      act.clickDb = Math.max(-40, Math.min(12, act.clickDb + 2 * dc));
      // The gaps set how much of the time is quiet. (Matching the count of seeks
      // instead fails on drives that seek back to back: the detector merges those.)
      work.gapMs = Math.max(0.2, work.gapMs * Math.pow((1 - real.busy + 0.01) / (1 - synth.busy + 0.01), 1.2));
    }
  }
  const r1 = (v) => Math.round(v * 10) / 10;
  act.trackMs = r1(act.trackMs); act.fullStrokeMs = r1(act.fullStrokeMs);
  act.modes.forEach((m) => { m.db = r1(m.db); });
  if (work.gapMs != null) work.gapMs = r1(work.gapMs);
  return { actuator: act, workload: work, real, synth };
}

// Third-octave difference of what the seeks add (dB), over the bands where the
// recording's seeks stand out.
// Both are floored 30 dB below the recording's strongest band.
function excessError(a, b, sr) {
  const cs = dsp.thirdOctaves(sr).filter((c) => c >= 100 && c <= sr * 0.42);
  const ra = cs.map((c) => excessAt(a, c)), rb = cs.map((c) => excessAt(b, c));
  const floor = Math.max(...ra) - 30;
  let e = 0;
  ra.forEach((v, i) => { e += Math.abs(Math.max(v, floor) - Math.max(rb[i], floor)); });
  return e / ra.length;
}

module.exports = { fitSeeks, measure, reference, detect, overIdle, excessError, findModes, HOP_S };
