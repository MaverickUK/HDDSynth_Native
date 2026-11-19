// What the engine costs, and whether a change to it can be heard.
//
//   node bench.js [--rate N] [--only NAME] [--ref OTHER/engine.js]
//
// For each fitted drive (drives/fitted) and design preset, busy with random seeks:
// - the time it takes here (ms per second of audio);
// - an estimate of its share of core 1 on the RP2350 (150 MHz). The engine counts its
//   own work (workDone()); each operation is priced in Cortex-M33 cycles (PRICE) and
//   the total scaled by CAL, so v1's C++ engine comes out at what it measured on the
//   board (about 1 ms per 4 ms block at 32 kHz). An estimate until v2 runs on the board.
//
// With --ref (another version of engine.js, e.g. from git show), it also renders
// spin-up, idle, random and sequential reads, bursts and spindown through both and
// subtracts them. Both must draw the same random numbers (true from the version with
// a generator per noise band), so the difference is exactly what the change did. It's
// measured against the drive's own sound in third-octave bands, over at least 24 FFT
// bins x frames (a noise's level read from fewer swings by +-10 dB). "worst" is the
// closest it comes to the sound anywhere: 20 dB or more under is safely unheard (a tone
// is heard down to about 5-7 dB under the noise in its band).
const fs = require('fs'), path = require('path');
const { makeEngine } = require('./engine.js');
const { designDrive, PRESETS } = require('./design.js');

const arg = (k, def) => { const i = process.argv.indexOf(k); return i > 0 ? process.argv[i + 1] : def; };
const SR = +arg('--rate', 32000), ONLY = arg('--only', ''), REF = arg('--ref', null);
const BLK = 128, MHZ = 150;
// Cycles on the M33, single-precision FPU, code in RAM (estimates): per sample, per
// sounding tone (a phasor), noise band (a random number and a filter) and arm filter
// stage; per control tick (1 kHz), the speed law and its powers, a body evaluation (a
// fitted curve: a log, a search and a power; designed: a few resonances), an exp, a
// retune (tan, as a table sin/cos) and each tone's turn, flutter and phase.
const PRICE = { sample: 40, tone: 16, band: 27, stage: 18, tick: 450, bodyCurve: 250, bodyModes: 60, exp: 60, retune: 59, turn: 30, flutter: 14, phase: 6, bandLevel: 8 };
// v1's engine by the same prices is 418 cycles a sample; it measured 1170 on the board.
const CAL = 1170 / 418;

const drives = {};
for (const f of fs.readdirSync(path.join(__dirname, 'drives', 'fitted')).sort()) {
  drives[f.replace('.json', '')] = JSON.parse(fs.readFileSync(path.join(__dirname, 'drives', 'fitted', f), 'utf8'));
}
for (const p of PRESETS) drives['design ' + p.id] = designDrive(p.design);

function busyEngine(make, d) {
  const e = make(SR); e.setParams(d); e.handle({ type: 'ready' }); e.handle({ type: 'busy', on: true, cls: 'random' });
  return e;
}
function msPerSecond(make, d) {
  const e = busyEngine(make, d), buf = new Float32Array(BLK), n = Math.round(5 * SR / BLK);
  for (let b = 0; b < n / 4; b++) e.process(buf, BLK);              // warm up
  const t0 = process.hrtime.bigint();
  for (let b = 0; b < n; b++) e.process(buf, BLK);
  return Number(process.hrtime.bigint() - t0) / 1e6 / 5;
}
// The estimated share of core 1, from the work the engine counted over 5 s busy.
function boardShare(d) {
  const e = busyEngine(makeEngine, d), buf = new Float32Array(BLK);
  for (let b = 0; b < SR / BLK; b++) e.process(buf, BLK);
  e.workDone();
  for (let b = 0; b < 5 * SR / BLK; b++) e.process(buf, BLK);
  const w = e.workDone(), P = PRICE;
  const body = (d.bodyCurve || []).length ? P.bodyCurve : P.bodyModes;
  const tones = w.tones / w.samples, nT = Math.min(48, (d.tones || []).length + ((d.bed && d.bed.count) || 0));
  const perTick = P.tick + (w.bodies * body + w.retunes * P.retune + w.exps * P.exp) / w.ticks
    + tones * (P.turn + (d.spindle.flutterDb ? P.flutter : 0)) + nT * P.phase + 18 * P.bandLevel;
  const perSample = P.sample + (w.tones * P.tone + w.bands * P.band + w.stages * P.stage) / w.samples + perTick * w.ticks / w.samples;
  return { share: perSample * CAL * SR / (MHZ * 1e6), tones, bands: w.bands / w.samples, stages: w.stages / w.samples };
}

// ---- subtracting two engines ----
// Each scenario is [message, seconds to render after it] steps.
function scenarios(d) {
  const up = Math.min(14, d.spindle.spinupS + 3), down = Math.min(16, (d.spindle.coastS || 8) + 1.5);
  const ready = { type: 'ready' }, busy = (cls) => ({ type: 'busy', on: true, cls }), idle = { type: 'busy', on: false };
  return {
    spinup: [[{ type: 'power', on: true }, up]],
    idle: [[ready, 4]],
    random: [[ready, 0.2], [busy('random'), 6]],
    sequential: [[ready, 0.2], [busy('sequential'), 4]],
    bursts: [[ready, 0.3], [busy('random'), 0.15], [idle, 0.6], [busy('random'), 0.08], [idle, 1.5]],
    spindown: [[ready, 0.5], [{ type: 'power', on: false }, down]],
  };
}
function render(make, d, steps) {
  const e = make(SR); e.setParams(d);
  const total = steps.reduce((a, s) => a + Math.round(s[1] * SR / BLK) * BLK, 0), x = new Float32Array(total), buf = new Float32Array(BLK);
  let o = 0;
  for (const [msg, secs] of steps) {
    e.handle(msg);
    for (let b = 0; b < Math.round(secs * SR / BLK); b++) { e.process(buf, BLK); x.set(buf, o); o += BLK; }
  }
  return x;
}
const N = 4096, win = Float64Array.from({ length: N }, (_, i) => 0.5 - 0.5 * Math.cos(2 * Math.PI * i / N));
const wsum = win.reduce((a, v) => a + v * v, 0);
function fft(re, im) {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i++) {
    let b = n >> 1; for (; j & b; b >>= 1) j ^= b; j ^= b;
    if (i < j) { [re[i], re[j]] = [re[j], re[i]]; [im[i], im[j]] = [im[j], im[i]]; }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const a = -2 * Math.PI / len, wr = Math.cos(a), wi = Math.sin(a), h = len / 2;
    for (let i = 0; i < n; i += len) {
      let cr = 1, ci = 0;
      for (let j = 0; j < h; j++) {
        const p = i + j, q = p + h, vr = re[q] * cr - im[q] * ci, vi = re[q] * ci + im[q] * cr;
        re[q] = re[p] - vr; im[q] = im[p] - vi; re[p] += vr; im[p] += vi;
        const t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
      }
    }
  }
}
const edges = []; for (let f = 25; f < SR / 2; f *= Math.pow(2, 1 / 3)) edges.push(f);
function thirds(x, o) {
  const re = new Float64Array(N), im = new Float64Array(N), p = new Float64Array(edges.length - 1);
  for (let i = 0; i < N; i++) re[i] = x[o + i] * win[i];
  fft(re, im);
  for (let k = 1; k < N / 2; k++) {
    const b = Math.floor(Math.log2(k * SR / N / 25) * 3);
    if (b >= 0 && b < p.length) p[b] += 2 * (re[k] * re[k] + im[k] * im[k]) / wsum / N;
  }
  return p;
}
const QUIET = Math.pow(10, -90 / 10);            // both under -90 dBFS in a band: not compared
function difference(ref, out) {
  const res = out.map((v, i) => v - ref[i]), frames = [];
  for (let o = 0; o + N <= ref.length; o += N / 2) frames.push([o, thirds(ref, o), thirds(res, o)]);
  let worst = -Infinity, at = '';
  for (let b = 0; b < edges.length - 1; b++) {
    const bins = Math.max(1, Math.round((edges[b + 1] - edges[b]) * N / SR)), span = Math.max(1, Math.ceil(24 / bins / 2) * 2 - 1);
    for (let f = 0; f + span <= frames.length; f++) {
      let ps = 0, pr = 0;
      for (let k = f; k < f + span; k++) { ps += frames[k][1][b]; pr += frames[k][2][b]; }
      if (ps / span < QUIET && pr / span < QUIET) continue;
      const v = 10 * Math.log10((pr + 1e-30) / (ps + 1e-30));
      if (v > worst) { worst = v; at = `${Math.round(edges[b] * Math.pow(2, 1 / 6))} Hz at ${(frames[f][0] / SR).toFixed(1)} s`; }
    }
  }
  return { worst, at };
}

const refMake = REF ? require(path.resolve(REF)).makeEngine : null;
console.log(`${SR} Hz, busy with random seeks.` + (REF ? ` Against ${REF}.` : ''));
let sum = 0, n = 0, worstAll = -Infinity;
for (const [name, d] of Object.entries(drives)) {
  if (ONLY && !name.includes(ONLY)) continue;
  const b = boardShare(d), ms = msPerSecond(makeEngine, d);
  sum += b.share; n++;
  let line = `${name.padEnd(22)} RP2350 ~${(b.share * 100).toFixed(0).padStart(3)}%  (${b.tones.toFixed(0)} tones, ${b.bands.toFixed(0)} bands, ${b.stages.toFixed(1)} arm stages)  here ${ms.toFixed(1)} ms/s`;
  if (refMake) {
    const msRef = msPerSecond(refMake, d), diffs = [];
    let w = { worst: -Infinity };
    for (const [k, steps] of Object.entries(scenarios(d))) {
      const r = difference(render(refMake, d, steps), render(makeEngine, d, steps));
      diffs.push(`${k} ${r.worst.toFixed(0)}`);
      if (r.worst > w.worst) w = { ...r, k };
    }
    worstAll = Math.max(worstAll, w.worst);
    line += ` (ref ${msRef.toFixed(1)})  difference: worst ${w.worst.toFixed(1)} dB (${w.k}, ${w.at}); ${diffs.join(', ')}`;
  }
  console.log(line);
}
console.log(`average RP2350 share ~${(sum / n * 100).toFixed(0)}% of core 1 (v1: ~${(418 * CAL * SR / (MHZ * 1e6) * 100).toFixed(0)}%)`);
if (refMake) console.log(`closest the difference comes to the drive's sound: ${worstAll.toFixed(1)} dB`);
