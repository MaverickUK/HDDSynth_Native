// Fits a drive's head events (heads.spinup and heads.spindown) from a pack's
// spinup.wav and spindown.wav.
//
// The recordings change speed all the way through, so a head event is found against a
// running median of each third-octave band (the background, ±200 ms): a short burst
// that stands 10 dB over it. The tones sweeping through a band also stand out, but in
// only a band or two and for longer, so an event must light four bands, or three if short.
//
// Each event found becomes a move of the drive's own actuator, at the same time. Its
// time and level are then set by rendering the events alone, adding them to the
// recording's background, and measuring that the same way, until the two agree.
const dsp = require('./dsp.js');
const { renderHeads } = require('./render.js');

const HOP_S = 0.002, WIN = 256, MEDIAN_HOPS = 100, ON_DB = 10, LIT_DB = 6;
const MAX_EVENTS = 128;
// The shortest move: shorter still, a voice coil's clicks (on, reverse, off) all but cancel.
const MIN_MS = 1;

// Third-octave band powers, frame by frame (2 ms hop).
function bandRows(x, sr) {
  const cs = dsp.thirdOctaves(sr).filter((c) => c >= 100 && c <= sr * 0.42);
  const n = Math.min(WIN, 1 << Math.round(Math.log2(sr * 0.016))), hop = Math.round(sr * HOP_S);
  const { S, df } = dsp.frames(x, sr, n, hop, 'hann');
  const bin = [];
  for (let k = 0; k * df < sr / 2; k++) bin.push(cs.findIndex((c) => k * df >= c / 1.122 && k * df < c * 1.122));
  return S.map((P) => { const r = new Float64Array(cs.length); for (let k = 0; k < P.length; k++) if (bin[k] >= 0) r[bin[k]] += P[k]; return r; });
}

// The background: each band's running median, taken every 10 frames and interpolated.
function background(R) {
  const nb = R.length ? R[0].length : 0, STEP = 10, pts = [];
  for (let i = 0; i < R.length; i += STEP) {
    const lo = Math.max(0, i - MEDIAN_HOPS), hi = Math.min(R.length, i + MEDIAN_HOPS), m = new Float64Array(nb);
    for (let b = 0; b < nb; b++) { const v = []; for (let k = lo; k < hi; k++) v.push(R[k][b]); v.sort((p, q) => p - q); m[b] = v[v.length >> 1]; }
    pts.push(m);
  }
  return R.map((_, i) => {
    const k = Math.min(pts.length - 1, Math.floor(i / STEP)), k1 = Math.min(pts.length - 1, k + 1), u = i / STEP - k;
    return pts[k].map((v, b) => v * (1 - u) + pts[k1][b] * u + 1e-20);
  });
}

// Runs of frames where add (the events) stands ON_DB over base in some band.
function runs(add, base) {
  const on = add.map((r, i) => r.some((p, b) => (p + base[i][b]) / base[i][b] >= Math.pow(10, ON_DB / 10)));
  const out = [];
  for (let i = 0; i < on.length;) {
    if (!on[i]) { i++; continue; }
    let j = i, gap = 0;
    while (j < on.length && gap < 3) { gap = on[j] ? 0 : gap + 1; j++; }
    out.push([i, j - gap]);
    i = j;
  }
  return out;
}

// The events in a recording: [{ a, b (frames), t (s), ms, e (their power over the background) }].
function findEvents(x, sr) {
  const R = bandRows(x, sr), base = background(R);
  const add = R.map((r, i) => r.map((p, b) => Math.max(0, p - base[i][b])));
  const ev = [];
  for (const [a, b] of runs(add, base)) {
    const ms = (b - a) * HOP_S * 1000;
    // How many bands it lights, on average over its frames.
    let lit = 0;
    for (let b2 = 0; b2 < base[0].length; b2++) {
      let s = 0, sb = 0;
      for (let k = a; k < b; k++) { s += R[k][b2]; sb += base[k][b2]; }
      if (s / sb >= Math.pow(10, LIT_DB / 10)) lit++;
    }
    if (!(lit >= 4 || (lit >= 3 && ms <= 24))) continue;
    ev.push({ a, b, t: a * HOP_S, ms, e: energy(add, a, b) });
  }
  // At most MAX_EVENTS: keep the strongest.
  if (ev.length > MAX_EVENTS) { ev.sort((p, q) => q.e - p.e); ev.length = MAX_EVENTS; ev.sort((p, q) => p.a - q.a); }
  return { ev, R, base };
}
// The power added over frames a..b and a little after (the ringing).
function energy(add, a, b) {
  let s = 0;
  for (let k = a; k < Math.min(add.length, b + 4); k++) for (const p of add[k]) s += p;
  return s;
}

// The synthesised events measured against the recording's background: each recorded
// event's power and duration, from the render, over the same frames.
function measureSynth(y, sr, real) {
  const S = bandRows(y, sr), n = Math.min(S.length, real.base.length);
  const add = S.slice(0, n), base = real.base.slice(0, n);
  const rr = runs(add, base);
  return real.ev.map((ev) => {
    // The synthesised run that starts nearest the event (within 10 ms).
    let best = null;
    for (const r of rr) if (Math.abs(r[0] - ev.a) <= 5 && (!best || Math.abs(r[0] - ev.a) < Math.abs(best[0] - ev.a))) best = r;
    return { e: ev.a < n ? energy(add, ev.a, Math.min(ev.b, n)) : 0, ms: best ? (best[1] - best[0]) * HOP_S * 1000 : 0 };
  });
}

// x: spinup.wav or spindown.wav; which: 'spinup' or 'spindown'. Returns the events
// ({ t, ms, db }, with x: 0 for the first spindown event of a voice coil: the park).
function fitScript(x, sr, drive, which) {
  if (!drive.actuator) return null;
  const real = findEvents(x, sr);
  if (!real.ev.length) return { events: [], real, synth: [] };
  const voice = drive.actuator.kind !== 'stepper';
  const events = real.ev.map((ev, i) => {
    const h = { t: Math.round(ev.t * 1000) / 1000, ms: Math.max(MIN_MS, ev.ms - 4), db: 0 };
    if (which === 'spindown' && i === 0 && voice) h.x = 0;
    return h;
  });
  const secs = x.length / sr;
  let synth = null;
  for (let round = 0; round < 5; round++) {
    const d = { ...drive, heads: { ...(drive.heads || {}), [which]: events } };
    synth = measureSynth(renderHeads(d, sr, which, secs), sr, real);
    events.forEach((h, i) => {
      const r = real.ev[i], s = synth[i];
      h.db += Math.max(-20, Math.min(20, 10 * Math.log10((r.e + 1e-20) / (s.e + 1e-20)))) * (round ? 0.8 : 1);
      h.db = Math.max(-40, Math.min(40, h.db));
      if (round < 4 && s.ms > 0) h.ms = Math.max(MIN_MS, Math.min(400, h.ms * Math.pow((r.ms + 2) / (s.ms + 2), 0.7)));
    });
  }
  events.forEach((h) => { h.ms = Math.round(h.ms * 10) / 10; h.db = Math.round(h.db * 10) / 10; });
  return { events, real, synth };
}

// How alike two recordings' head events are: the events of each, matched by time
// (within 20 ms), and the mean level difference of the matched ones (dB).
function compareEvents(real, synth, sr) {
  const a = findEvents(real, sr).ev, b = findEvents(synth, sr).ev;
  let hit = 0, err = 0;
  for (const ev of a) {
    const m = b.find((s) => Math.abs(s.a - ev.a) <= 10);
    if (m) { hit++; err += Math.abs(10 * Math.log10((m.e + 1e-20) / (ev.e + 1e-20))); }
  }
  return { real: a.length, synth: b.length, matched: hit, levelErr: hit ? err / hit : null };
}

module.exports = { fitScript, findEvents, compareEvents, HOP_S };
