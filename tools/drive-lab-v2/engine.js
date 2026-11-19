// HDD Synth Drive Lab v2: the drive model and synthesis engine.
//
// makeEngine() is self-contained (no outer references) so its source can be sent
// to an AudioWorklet with Function.toString(), and it's written in plain,
// fixed-structure code that ports to C++ for the RP2350.
//
// The spindle is a source-filter model, built from what the real recordings show:
//   - tones at fixed multiples ("orders") of the rotation rate. Each drive has its
//     own set (the Raptor's are at 1, 4, 13, 16 and 24 x 166.7 Hz). Orders may be
//     fractional (bearing defects, motor drive frequencies).
//   - a body: a few fixed resonances that every tone passes through. As the speed
//     changes, the tones sweep through them and light up in turn.
//   - air noise in half-octave bands, louder and brighter with speed.
// Everything is a parameter of the drive, so a drive can be designed from scratch
// or fitted from a recording (fit.js) and then edited.
//
// The actuator (seeks) is the same idea: an excitation through a few fixed
// resonances (the arm and its mount: the low "rock" of a seek, 200-500 Hz on most
// drives, and higher ringing modes). A voice-coil seek pushes the arm one way then
// the other (bang-bang), taking longer the further it goes; a stepper moves in
// steps at its step rate. While the drive is busy, a workload picks the seeks.

function makeEngine(sr) {
  const TAU = Math.PI * 2, dt = 1 / sr;
  const CTRL_HZ = 1000, ctrlEvery = Math.max(1, Math.round(sr / CTRL_HZ)), ctrlDt = ctrlEvery / sr;
  const MAX_TONES = 48, MAX_BODY = 8;
  // Half-octave noise bands, 40 Hz to 14.5 kHz. Q gives each band a -3 dB width of half an octave.
  const NB = 18, NOISE_Q = 2.87;
  const noiseHz = new Float64Array(NB);
  for (let i = 0; i < NB; i++) noiseHz[i] = 40 * Math.pow(2, i / 2);

  // ---- small helpers ----
  let seed = 0x2545F491;
  const rnd = () => { seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5; return (seed >>> 0) / 4294967296; };
  const SQRT3 = Math.sqrt(3);
  const noise = () => (rnd() * 2 - 1) * SQRT3;          // unit variance
  // Each noise band has its own generator, so a band stopping or starting doesn't
  // change the others' noise, and the control's random numbers (flutter, seeks) don't
  // depend on how many bands are running.
  const bSeed = new Uint32Array(NB);
  for (let i = 0; i < NB; i++) bSeed[i] = (Math.imul(0x9E3779B9, i + 1) >>> 0) || 1;
  const bandNoise = (i) => { let s = bSeed[i] | 0; s ^= s << 13; s ^= s >>> 17; s ^= s << 5; bSeed[i] = s; return ((s >>> 0) / 4294967296 * 2 - 1) * SQRT3; };
  const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);
  const dbToAmp = (db) => Math.pow(10, db / 20);

  // State-variable filter (TPT form): stays stable while its cutoff moves.
  function svf() { return { a1: 0, a2: 0, a3: 0, k: 1, ic1: 0, ic2: 0, bp: 0 }; }
  function tuneSvf(s, fc, q) {
    const g = Math.tan(Math.PI * clamp(fc, 10, sr * 0.45) / sr);
    s.k = 1 / q; s.a1 = 1 / (1 + g * (g + s.k)); s.a2 = g * s.a1; s.a3 = g * s.a2;
  }
  function runSvf(s, x) {
    const v3 = x - s.ic2, v1 = s.a1 * s.ic1 + s.a2 * v3, v2 = s.ic2 + s.a2 * s.ic1 + s.a3 * v3;
    s.ic1 = 2 * v1 - s.ic1; s.ic2 = 2 * v2 - s.ic2; s.bp = v1;
  }

  // ---- parameters (copied out of the drive object into flat arrays) ----
  let d = null;
  let nTones = 0;
  const tOrder = new Float64Array(MAX_TONES), tAmp = new Float64Array(MAX_TONES), tMotor = new Uint8Array(MAX_TONES);
  let nBody = 0, nCurve = 0;
  const bHz = new Float64Array(MAX_BODY), bQ = new Float64Array(MAX_BODY), bGain = new Float64Array(MAX_BODY);
  const MAX_CURVE = 40;
  const cLogHz = new Float64Array(MAX_CURVE), cDb = new Float64Array(MAX_CURVE);
  const nRms = new Float64Array(NB);   // each noise band's RMS at full speed
  // Actuator: arm/mount modes and the excitation mix.
  const MAX_MODES = 8;
  let nModes = 0;
  // Each mode is two band-pass stages in series: real seeks fall away steeply either
  // side of their modes, and one stage's skirts (6 dB/octave) smear them everywhere.
  const modes = [], modes2 = []; for (let i = 0; i < MAX_MODES; i++) { modes.push(svf()); modes2.push(svf()); }
  const STAGE_Q = 0.644;   // two stages at Q * 0.644 have the -3 dB width of one at Q
  const mGain = new Float64Array(MAX_MODES);
  let act = null, thumpAmp = 0, buzzAmp = 0, clickAmp = 0, seekAmp = 0;

  // The body curve's gain at f (dB): straight lines between its points on a log-frequency axis.
  function curveDb(f) {
    if (nCurve === 0) return 0;
    const lf = Math.log(f);
    if (lf <= cLogHz[0]) return cDb[0];
    for (let i = 1; i < nCurve; i++) {
      if (lf <= cLogHz[i]) return cDb[i - 1] + (cDb[i] - cDb[i - 1]) * (lf - cLogHz[i - 1]) / (cLogHz[i] - cLogHz[i - 1]);
    }
    return cDb[nCurve - 1];
  }

  // Body response at f (linear amplitude): 1 plus a resonance curve per mode,
  // times the body curve (fitted drives use the curve, designed ones the modes).
  function body(f) {
    let h = 1;
    for (let i = 0; i < nBody; i++) {
      const r = f / bHz[i] - bHz[i] / f;
      h += (bGain[i] - 1) / (1 + bQ[i] * bQ[i] * r * r);
    }
    if (nCurve) h *= dbToAmp(curveDb(f));
    return h > 0 ? h : 0;
  }

  // ---- state ----
  let powered = false, state = 'off';           // off | spinup | ready | coast
  let rpm = 0, accel = 0, wander = 0, frNow = 0;
  // Each tone is a phasor (re, im) turned by (tC, tS) every sample: im is the sine.
  // Four multiplies instead of a sin() per tone per sample. The turn is set at the
  // control rate, as the speed only changes there anyway.
  const tPh = new Float64Array(MAX_TONES), tCur = new Float64Array(MAX_TONES), tStep = new Float64Array(MAX_TONES);
  const tRe = new Float64Array(MAX_TONES), tIm = new Float64Array(MAX_TONES), tC = new Float64Array(MAX_TONES).fill(1), tS = new Float64Array(MAX_TONES);
  // Each tone's body gain, kept until the speed changes: it's the dearest part of the
  // control (a log and a power per tone on a fitted drive's curve).
  const tBodyF = new Float64Array(MAX_TONES).fill(-1), tBody = new Float64Array(MAX_TONES);
  const tOn = new Uint8Array(MAX_TONES), tList = new Uint8Array(MAX_TONES);   // the tones sounding now
  let nList = 0;
  let rotRe = 1, rotIm = 0, rotC = 1, rotS = 0;  // the rotation itself (the wobble), as a phasor
  const bands = []; for (let i = 0; i < NB; i++) bands.push(svf());
  const nCur = new Float64Array(NB), nStep = new Float64Array(NB);
  // The bands' tuning and body gain, kept until the speed changes (nShift: their shift then).
  let nShift = -1;
  const bandBody = new Float64Array(NB), nNorm = new Float64Array(NB);
  const bList = new Uint8Array(NB), bOn = new Uint8Array(NB);   // the bands sounding now
  let nbList = 0;
  let armLive = false;                           // the arm's modes are still ringing
  let master = 1, wobK = 0;
  let ctrlCount = 0;
  // Work done, for the cost estimate (bench): per-sample and per-control-tick counts.
  const work = { samples: 0, tones: 0, bands: 0, stages: 0, ticks: 0, bodies: 0, retunes: 0, exps: 0 };

  function setParams(drive) {
    d = drive;
    const tones = (drive.tones || []).concat(bedTones(drive.bed, (drive.tones || []).length));
    const had = nTones;
    nTones = Math.min(tones.length, MAX_TONES);
    for (let i = 0; i < nTones; i++) {
      tOrder[i] = tones[i].order; tAmp[i] = dbToAmp(tones[i].db); tMotor[i] = tones[i].motor ? 1 : 0;
      // Random start phases (no click of aligned sines), only for new tones: an edit
      // while playing keeps the others running smoothly (their levels ramp).
      if (i >= had) { tPh[i] = rnd(); tCur[i] = 0; tRe[i] = Math.cos(TAU * tPh[i]); tIm[i] = Math.sin(TAU * tPh[i]); }
      tBodyF[i] = -1;                                  // the body may have changed: work it out again
      setTurn(i);
    }
    for (let i = nTones; i < had; i++) { tCur[i] = 0; tStep[i] = 0; }
    const sp = drive.spindle || {};
    setFlutter(sp.flutterDb, sp.flutterHz);
    const b = drive.body || [];
    nBody = Math.min(b.length, MAX_BODY);
    for (let i = 0; i < nBody; i++) { bHz[i] = b[i].hz; bQ[i] = b[i].q; bGain[i] = dbToAmp(b[i].db); }
    const c = drive.bodyCurve || [];                   // [{hz, db}], rising hz
    nCurve = Math.min(c.length, MAX_CURVE);
    for (let i = 0; i < nCurve; i++) { cLogHz[i] = Math.log(c[i].hz); cDb[i] = c[i].db; }
    const nb = (drive.noise && drive.noise.db) || [];
    for (let i = 0; i < NB; i++) nRms[i] = i < nb.length ? dbToAmp(nb[i]) : 0;
    nShift = -1;                                       // retune the bands at the next control
    master = drive.master == null ? 1 : drive.master;
    wobK = (sp.wobble || 0) * 0.5;

    act = drive.actuator || null;
    const am = (act && act.modes) || [];
    nModes = Math.min(am.length, MAX_MODES);
    for (let i = 0; i < nModes; i++) {
      const q = Math.max(0.5, am[i].q) * STAGE_Q;
      tuneSvf(modes[i], am[i].hz, q); tuneSvf(modes2[i], am[i].hz, q);
      // Peak gain 1 at the mode (k * bp), scaled so a mode's db sets its level for a
      // broadband excitation regardless of its Q: narrow modes would otherwise be quieter.
      mGain[i] = dbToAmp(am[i].db) * Math.sqrt(am[i].q / 4);
    }
    setGrains();
    setHeads(drive.heads);
    // heads.rest: where the head sits with the power off (-0.06 on a ramp just off the
    // platter, 1 on the landing zone at the hub). Moved there only while it's off.
    if (state === 'off' && !sk && drive.heads && drive.heads.rest != null) headX = drive.heads.rest;
    if (act) {
      seekAmp = dbToAmp(act.db == null ? 0 : act.db);
      thumpAmp = act.thumpDb == null ? 0 : dbToAmp(act.thumpDb);
      buzzAmp = act.buzzDb == null ? 0 : dbToAmp(act.buzzDb);
      clickAmp = act.clickDb == null ? 0 : dbToAmp(act.clickDb);
    }
  }

  // A bed of faint tones, for a designed drive: a real one has dozens besides its
  // main ones, many off the whole orders (bearings, the motor's drive). bed:
  // { count, db (their average level), maxOrder, fractional (share off whole
  // orders), seed }. The same seed always gives the same bed.
  function bedTones(bed, have) {
    if (!bed || !bed.count) return [];
    let s = (bed.seed || 1) * 2654435761 >>> 0 || 1;
    const r = () => { s ^= s << 13; s ^= s >>> 17; s ^= s << 5; return (s >>> 0) / 4294967296; };
    const out = [], maxO = bed.maxOrder || 60, n = Math.min(bed.count, MAX_TONES - have);
    for (let i = 0; i < n; i++) {
      // Spread evenly on a log scale of order, so low orders aren't crowded out.
      let order = Math.exp(Math.log(maxO) * r());
      order = r() < (bed.fractional == null ? 0.3 : bed.fractional) ? Math.round(order * 1000) / 1000 : Math.max(1, Math.round(order));
      out.push({ order, db: (bed.db == null ? -60 : bed.db) + (r() - 0.5) * 12 });
    }
    return out;
  }

  // Flutter: each tone's level wanders slowly and independently, by
  // spindle.flutterDb (its standard deviation). Real tones flicker by 1.5-3.5 dB
  // over 50 ms (air currents, the tones beating with their reflections).
  // Two one-pole low-passes in series: with one, the flutter's own fast changes spread
  // each loud tone into a skirt hundreds of Hz wide that filled the quiet bands.
  // spindle.flutterHz is how fast it wanders (default 2 Hz): faster flutter on a loud
  // tone also fills the quiet bands beside it.
  let flutterK = 0, flA = 0, flNorm = 1;
  const tFl = new Float64Array(MAX_TONES), tFl2 = new Float64Array(MAX_TONES);
  function setFlutter(db, hz) {
    flutterK = (db || 0) / 20 * Math.LN10;          // flutter in dB as a natural-log gain
    flA = Math.exp(-TAU * clamp(hz || 2, 0.1, 50) * ctrlDt);
    // Unit white noise through two stages of (1 - a) / (1 - a z^-1) has variance
    // (1 - a)^4 (1 + a^2) / (1 - a^2)^3: scale it back to 1.
    flNorm = 1 / Math.sqrt(Math.pow(1 - flA, 4) * (1 + flA * flA) / Math.pow(1 - flA * flA, 3));
  }

  // ---- the actuator and the workload ----
  // The head's position x runs 0 (outer edge) to 1 (inner). A voice-coil seek of
  // distance d takes trackMs + (fullStrokeMs - trackMs) sqrt(d): the arm accelerates
  // for half the time and brakes for the other half. A stepper takes one step per
  // 1/stepHz, strokeSteps for the full stroke.
  let headX = 0.1, sk = null;            // sk: the seek in progress
  let busy = false, busyCls = 'random', gapLeft = 0, seqLeft = 0, pending = null;
  let thump = 0;
  const thumpK = 1 - Math.exp(-dt / 0.001);
  const isStepper = () => act && act.kind === 'stepper';
  const settleS = () => Math.max(0.0005, (act.settleMs == null ? 3 : act.settleMs) / 1000);
  function seekTimeS(dist) {
    if (isStepper()) {
      const n = Math.max(1, Math.round(dist * (act.strokeSteps || 300)));
      return n / (act.stepHz || 250);
    }
    const t0 = act.trackMs == null ? 3 : act.trackMs, t1 = act.fullStrokeMs == null ? 25 : act.fullStrokeMs;
    return (t0 + (t1 - t0) * Math.sqrt(dist)) / 1000;
  }
  // The distance a seek of T seconds covers (the time law turned round).
  function distForS(T) {
    if (isStepper()) return clamp(T * (act.stepHz || 250) / (act.strokeSteps || 300), 0, 1);
    const t0 = act.trackMs == null ? 3 : act.trackMs, t1 = act.fullStrokeMs == null ? 25 : act.fullStrokeMs;
    const u = clamp((T * 1000 - t0) / Math.max(0.1, t1 - t0), 0, 1);
    return u * u;
  }
  // Seek to x. A head event (heads.spinup/spindown) instead gives the move's time
  // (forceS) and a level (db): with x null it goes as far as that time takes it,
  // inwards from the outer half and outwards from the inner.
  function startSeek(x, forceS, db) {
    if (!act) return;
    if (x == null) { const dd = distForS(forceS); x = headX < 0.5 ? headX + dd : headX - dd; }
    x = clamp(x, -0.1, 1.05);                        // a little past the data: the ramp, the landing zone
    const dist = Math.abs(x - headX);
    const stepper = isStepper(), stepHz = act.stepHz || 250;
    const n = stepper ? Math.max(1, Math.round(forceS != null ? forceS * stepHz : dist * (act.strokeSteps || 300))) : 0;
    const T = stepper ? n / stepHz : forceS != null ? Math.max(0.0002, forceS) : seekTimeS(dist), Tfull = seekTimeS(1);
    // Longer seeks push harder: the excitation grows with the seek's length (a slow
    // move longer than a full stroke doesn't push harder still).
    const A = Math.pow(Math.min(1, T / Tfull), act.levelExp == null ? 0.5 : act.levelExp) * (db ? dbToAmp(db) : 1);
    sk = { x0: headX, x1: x, T, A, t: 0, n, step: 0, hold: 0 };
    armLive = true;
    // How long a seek this is among random ones: two random points are further apart
    // than d with probability (1 - d)^2. The grains are in order of length, so this
    // picks short ones for short seeks, as often as the recording had them.
    if (grainMix > 0) sk.hold = startGrain(1 - (1 - dist) * (1 - dist)) / sr;
  }

  // ---- recorded seeks (grains), optional ----
  // drive.grains.items lists each grain's place in the buffer (sent with a 'grains'
  // message) and its length as measured (ms). Each seek plays one of the three at
  // its rank among the grains' lengths (not the one just played), on one of two
  // voices so a grain still ringing isn't cut off. The head waits for the grain.
  let gBuf = null, gItems = [], grainMix = 0, gLast = -1;
  const gPos = [0, 0], gEnd = [0, 0];
  let gNext = 0;
  function setGrains() {
    const g = d && d.grains;
    gItems = g && g.items && gBuf ? g.items.slice().sort((p, q) => p.ms - q.ms) : [];
    grainMix = gItems.length ? clamp(g.mix == null ? 1 : g.mix, 0, 1) : 0;
    gSynthGain = Math.cos(grainMix * Math.PI / 2); gGrainGain = Math.sin(grainMix * Math.PI / 2);
  }
  let gSynthGain = 1, gGrainGain = 0;
  // rank: 0 (shortest) to 1 (longest). Returns the grain's length in samples.
  function startGrain(rank) {
    const best = Math.round(clamp(rank, 0, 1) * (gItems.length - 1));
    const lo = Math.max(0, Math.min(best - 1, gItems.length - 3)), hi = Math.min(gItems.length - 1, lo + 2);
    let k = lo + Math.floor(rnd() * (hi - lo + 1));
    if (k === gLast && hi > lo) k = k === hi ? lo : k + 1;
    gLast = k;
    const it = gItems[k], v = gPos[gNext] < gEnd[gNext] ? 1 - gNext : gNext;   // a free voice if there is one
    gPos[v] = it.start; gEnd[v] = Math.min(gBuf.length, it.start + it.len);
    gNext = 1 - v;
    return gEnd[v] - gPos[v];
  }
  function grainSample() {
    let y = 0;
    for (let v = 0; v < 2; v++) if (gPos[v] < gEnd[v]) y += gBuf[gPos[v]++];
    return y;
  }
  // Where the head is during a seek: a stepper moves evenly; a voice coil follows the
  // bang-bang curve (quadratic up to half way, then quadratic down).
  function headAt(s) {
    const u = clamp(s.t / s.T, 0, 1);
    const p = isStepper() ? u : u < 0.5 ? 2 * u * u : 1 - 2 * (1 - u) * (1 - u);
    return s.x0 + (s.x1 - s.x0) * p;
  }
  // The workload, at the control rate: while busy, after each seek wait for the
  // sector to come round (0 to 1 revolution) and the transfer (gapMs on average),
  // then seek again. Random seeks go anywhere; with locality some stay close.
  // Sequential reading steps one track every seqRevs revolutions.
  function workload() {
    if (!act) return;
    // The head is free once it has settled; the tail may still be ringing.
    if (sk && sk.t < Math.max(sk.T + settleS(), sk.hold)) return;
    if (pending != null) { startSeek(pending); pending = null; return; }
    if (!busy) return;
    if (gapLeft > 0) { gapLeft -= ctrlDt; return; }
    const w = d.workload || {}, rev = rpm > 0 ? 60 / rpm : 0.01;
    // seqShare: how much of a random workload is sequential runs (1: all of it).
    const share = w.seqShare || 0;
    const seq = busyCls === 'sequential' || share >= 1 || seqLeft > 0;
    if (seq) {
      startSeek(headX + (headX < 0.999 ? 0.0005 : -0.5));
      // The next step comes seqRevs revolutions after this one starts.
      gapLeft = Math.max(0, (w.seqRevs || 2) * rev - seekTimeS(0.0005) - settleS());
      if (seqLeft > 0) seqLeft -= 1;
      return;
    }
    if (share > 0 && rnd() < share * 0.1) seqLeft = 10 + Math.floor(rnd() * 30);
    const loc = w.locality == null ? 0.2 : w.locality;
    const x = rnd() < loc ? clamp(headX + (rnd() - 0.5) * 0.04, 0, 1) : rnd();   // on the data, not the ramp
    startSeek(x);
    gapLeft = rnd() * rev - Math.log(1 - rnd() * 0.999) * (w.gapMs == null ? 4 : w.gapMs) / 1000;
  }

  // ---- head events ----
  // What a drive does with its heads at power on and off, as two scripts of moves:
  // heads.spinup (seconds after power on: the motor's start-up kicks, then loading the
  // heads and calibrating) and heads.spindown (seconds after power off: the park, and
  // any knocks as the heads land or latch). Each event is { t, ms, db, x? }: when, the
  // move's time, its level against an ordinary seek of that time, and optionally where
  // to (0 is the outer edge, where a voice coil parks). While a script plays, the
  // workload waits for it.
  const MAX_EVENTS = 128;
  const hT = [new Float64Array(MAX_EVENTS), new Float64Array(MAX_EVENTS)];
  const hMs = [new Float64Array(MAX_EVENTS), new Float64Array(MAX_EVENTS)];
  const hDb = [new Float64Array(MAX_EVENTS), new Float64Array(MAX_EVENTS)];
  const hX = [new Float64Array(MAX_EVENTS), new Float64Array(MAX_EVENTS)];   // NaN: no x given
  const hN = [0, 0], hEnd = [0, 0];
  let scr = -1, scrT = 0, scrI = 0;                // the script playing (0 spinup, 1 spindown), its clock and next event
  function setHeads(h) {
    ['spinup', 'spindown'].forEach((k, s) => {
      const ev = ((h && h[k]) || []).slice().sort((p, q) => p.t - q.t);
      hN[s] = Math.min(ev.length, MAX_EVENTS); hEnd[s] = 0;
      for (let i = 0; i < hN[s]; i++) {
        hT[s][i] = Math.max(0, ev[i].t); hMs[s][i] = Math.max(0.2, ev[i].ms == null ? 5 : ev[i].ms);
        hDb[s][i] = ev[i].db || 0; hX[s][i] = ev[i].x == null ? NaN : ev[i].x;
        hEnd[s] = Math.max(hEnd[s], hT[s][i] + hMs[s][i] / 1000);
      }
    });
  }
  function startScript(s) { scr = act && hN[s] ? s : -1; scrT = 0; scrI = 0; }
  function runScript() {
    if (scr < 0) return;
    scrT += ctrlDt;
    while (scrI < hN[scr] && scrT >= hT[scr][scrI]) {
      const i = scrI++;
      startSeek(isNaN(hX[scr][i]) ? null : hX[scr][i], hMs[scr][i] / 1000, hDb[scr][i]);
    }
    if (scrI >= hN[scr] && scrT >= hEnd[scr] + settleS()) scr = -1;
  }

  // One sample of the actuator's excitation (then filtered by the modes).
  function seekSample() {
    if (!sk) return 0;
    const s = sk, A = s.A;
    const settle = settleS();
    let e = 0, f = 0, env;
    if (isStepper()) {
      // One jolt per step, and the coil current while stepping.
      const stepEvery = 1 / (act.stepHz || 250);
      if (s.step < s.n && s.t >= s.step * stepEvery) { e += clickAmp * A; s.step++; }
      f = s.t < s.T ? A : 0;
      env = s.t < s.T ? A : A * Math.exp(-(s.t - s.T) / settle);
    } else {
      // The force's edges are the clicks: on at the start, reversed half way, off at the end.
      const h = s.T / 2;
      if (s.t === 0) e += clickAmp * A;
      else if (s.t - dt < h && s.t >= h) e -= 2 * clickAmp * A;
      else if (s.t - dt < s.T && s.t >= s.T) e += clickAmp * A;
      f = s.t < h ? A : s.t < s.T ? -A : 0;
      env = s.t < s.T ? A : A * Math.exp(-(s.t - s.T) / settle);
    }
    thump += (f - thump) * thumpK;                  // the force itself, softened over ~1 ms
    e += thump * thumpAmp + env * noise() * buzzAmp; // plus the coil's current ripple (buzz)
    s.t += dt;
    headX = headAt(s);
    if (s.t > Math.max(s.T + 6 * settle, s.hold)) sk = null;   // the tail has died away (and any grain)
    return e;
  }

  // The coast-down: ds/dt = -(s / tau + fric), s the fraction of full speed.
  // spindle.friction (0..1) is friction's share of the deceleration at full speed:
  // near 0 the drive slows like a decaying exponential (windage), at 1 in a straight
  // line (constant friction). tau and fric are set so it stops at exactly coastS.
  // (The same law is in fit.js and spindown_fit.js.)
  function coastConsts(s) {
    const k = clamp(s.friction == null ? 0.082 : s.friction, 0.01, 0.999);
    const tau = s.coastS / Math.log(1 + (1 - k) / k);
    return { tau, fric: k / ((1 - k) * tau) };
  }

  // Runs at CTRL_HZ: the spindle speed, then every tone's and band's target level.
  function control() {
    const s = d.spindle, target = s.rpm;
    // Every tone's phase, kept at the control rate (the speed it ran at since the last
    // tick), heard or not: a tone left out comes back where it would have been.
    for (let i = 0; i < nTones; i++) { tPh[i] += tOrder[i] * frNow * ctrlDt; tPh[i] -= Math.floor(tPh[i]); }
    if (powered) {
      if (rpm < target) {
        // Constant torque, less the rising windage drag: reaches full speed in about spinupS.
        rpm += (target / s.spinupS) * ctrlDt * (1.25 - 0.5 * rpm / target);
        if (rpm >= target) { rpm = target; }
        accel += (1 - accel) * 0.05;
      } else {
        rpm += (target - rpm) * 0.02;
        accel += (0 - accel) * 0.01;
      }
      if (state === 'spinup' && rpm >= target * 0.999) state = 'ready';
    } else {
      // Windage (proportional to speed) plus bearing friction (constant): stops at exactly coastS.
      const c = coastConsts(s);
      rpm -= ctrlDt * target * (rpm / target / c.tau + c.fric);
      if (rpm <= 0) { rpm = 0; state = 'off'; }
      accel *= 0.97;
    }
    // Slow random wander of the speed: no motor holds it perfectly steady.
    wander = wander * 0.995 + noise() * 0.1;
    frNow = (rpm / 60) * (1 + (s.jitter || 0) * wander);

    const sn = target > 0 ? rpm / target : 0;
    const nyq = sr * 0.45, fade0 = sr * 0.4;
    work.ticks++;

    // The noise bands' levels. (Leaving out bands or tones masked by louder sound nearby
    // was tried, and measured: kept far enough under to be sure they're unheard, it
    // saved 0-3 of 17 bands and 0-2 of 40 tones. Everything a drive has is heard.)
    const nz = d.noise || {};
    const nLaw = Math.pow(sn, nz.speedExp == null ? 2.5 : nz.speedExp);
    const shift = Math.pow(Math.max(sn, 0.05), nz.follow == null ? 0.5 : nz.follow);
    if (Math.abs(shift - nShift) > 1e-5 * shift) {   // the speed has changed: retune them
      nShift = shift;
      for (let i = 0; i < NB; i++) {
        const fc = noiseHz[i] * shift;
        bandBody[i] = 0;
        if (fc < nyq && nRms[i] > 0) {
          tuneSvf(bands[i], fc, NOISE_Q);
          bandBody[i] = body(fc);
          // A unit-peak band-pass passes pi*fc/(Q*sr) of unit white noise's power.
          nNorm[i] = 1 / Math.sqrt(Math.PI * fc / (NOISE_Q * sr));
          work.retunes++; work.bodies++;
        }
      }
    }
    nbList = 0;
    for (let i = 0; i < NB; i++) {
      const g = nRms[i] * nLaw * bandBody[i] * nNorm[i];
      if (g === 0 && Math.abs(nCur[i]) < 1e-12) { nCur[i] = 0; nStep[i] = 0; }
      else nStep[i] = (g - nCur[i]) / ctrlEvery;
      if (nCur[i] !== 0 || nStep[i] !== 0) { bList[nbList++] = i; bOn[i] = 1; }
      else if (bOn[i]) {
        // Gone quiet (stopped, or past the top of the range): clear its filter, so it
        // comes back from silence. Its old state, retuned meanwhile, can make a thump.
        bOn[i] = 0; const b = bands[i]; b.ic1 = 0; b.ic2 = 0; b.bp = 0;
      }
    }

    const mech = Math.pow(sn, s.speedExp == null ? 2 : s.speedExp);
    // Motor tones follow the drive current: louder while accelerating, gone once unpowered.
    // They also grow with speed (as the recordings' spin-ups do: they start 10-25 dB
    // under the idle). At full level from 10% speed, they were a loud whirr.
    const motor = powered ? (1 + (dbToAmp(s.motorBoostDb || 0) - 1) * accel) * sn : 0;
    // The body is worked out at the steady speed, without the jitter (0.05%): it moves
    // the tones' pitch, not their level.
    const fBase = rpm / 60;
    nList = 0;
    for (let i = 0; i < nTones; i++) {
      const f = tOrder[i] * frNow;
      let a = 0;
      if (f > 1 && f < nyq) {
        const fb = tOrder[i] * fBase;
        if (Math.abs(fb - tBodyF[i]) > 1e-4 * fb) { tBodyF[i] = fb; tBody[i] = body(fb); work.bodies++; }
        a = tAmp[i] * (tMotor[i] ? motor : mech) * tBody[i];
        if (f > fade0) a *= (nyq - f) / (nyq - fade0);
        // (exp(-k^2) keeps the tone's average power where it was.)
        if (flutterK) {
          tFl[i] += (noise() - tFl[i]) * (1 - flA);
          tFl2[i] += (tFl[i] - tFl2[i]) * (1 - flA);
          a *= Math.exp(flutterK * tFl2[i] * flNorm - flutterK * flutterK);
          work.exps++;
        }
      }
      // Only the tones sounding run per sample (a motor tone is silent unpowered, a tone
      // past the top of the range is gone).
      if (a === 0 && Math.abs(tCur[i]) < 1e-12) { tCur[i] = 0; tStep[i] = 0; }
      else tStep[i] = (a - tCur[i]) / ctrlEvery;
      if (tCur[i] !== 0 || tStep[i] !== 0) {
        // A tone coming back joins at the phase it would have had: no click.
        if (!tOn[i]) { tOn[i] = 1; tRe[i] = Math.cos(TAU * tPh[i]); tIm[i] = Math.sin(TAU * tPh[i]); }
        tList[nList++] = i; setTurn(i);
      } else tOn[i] = 0;
    }
    { const w = TAU * frNow * dt; rotC = Math.cos(w); rotS = Math.sin(w); }
    // Renormalise the phasors now and then: rounding slowly changes their size.
    if ((work.ticks & 63) === 0) {
      for (let i = 0; i < nTones; i++) { const m = 1.5 - 0.5 * (tRe[i] * tRe[i] + tIm[i] * tIm[i]); tRe[i] *= m; tIm[i] *= m; }
      const m = 1.5 - 0.5 * (rotRe * rotRe + rotIm * rotIm); rotRe *= m; rotIm *= m;
    }
    // The arm's modes stop once the seek is over and they've rung down to nothing.
    if (armLive && !sk) {
      let e = 0;
      for (let i = 0; i < nModes; i++) e = Math.max(e, Math.abs(modes[i].ic1), Math.abs(modes[i].ic2), Math.abs(modes2[i].ic1), Math.abs(modes2[i].ic2));
      if (e < 1e-7) {
        armLive = false;
        for (let i = 0; i < nModes; i++) for (const m of [modes[i], modes2[i]]) { m.ic1 = 0; m.ic2 = 0; m.bp = 0; }
      }
    }
    runScript();
    if (state === 'ready' && scr < 0) workload();
  }

  function setTurn(i) { const w = TAU * tOrder[i] * frNow * dt; tC[i] = Math.cos(w); tS[i] = Math.sin(w); }

  function power(on) {
    if (on === powered) return;
    powered = on;
    state = on ? 'spinup' : 'coast';
    startScript(on ? 0 : 1);
  }

  function handle(m) {
    if (m.type === 'params') setParams(m.drive);
    else if (m.type === 'power') power(m.on);
    else if (m.type === 'ready') { powered = true; rpm = d.spindle.rpm; accel = 0; state = 'ready'; scr = -1; } // jump to full speed (renders, tests)
    else if (m.type === 'busy') { busy = !!m.on; if (m.cls) busyCls = m.cls; if (!busy) seqLeft = 0; }
    else if (m.type === 'seek') pending = m.x;      // seek to x (0..1) once the head is free
    else if (m.type === 'grains') { gBuf = m.data || null; setGrains(); }   // the drive's grains file, as samples
  }

  let peak = 0, sq = 0, n = 0;
  function process(out, len) {
    for (let j = 0; j < len; j++) {
      if (++ctrlCount >= ctrlEvery) {
        ctrlCount = 0; control();
        work.tones += nList * ctrlEvery; work.bands += nbList * ctrlEvery;
      }
      work.samples++;
      let wob = 1;
      if (wobK) {
        const re = rotRe * rotC - rotIm * rotS; rotIm = rotRe * rotS + rotIm * rotC; rotRe = re;
        wob = 1 + wobK * rotIm;
      }

      let tones = 0;
      for (let k = 0; k < nList; k++) {
        const i = tList[k], re = tRe[i], im = tIm[i];
        const ni = re * tS[i] + im * tC[i];
        tRe[i] = re * tC[i] - im * tS[i]; tIm[i] = ni;
        tCur[i] += tStep[i];
        tones += tCur[i] * ni;
      }
      let air = 0;
      for (let k = 0; k < nbList; k++) {
        const i = bList[k], b = bands[i];
        nCur[i] += nStep[i];
        runSvf(b, bandNoise(i));
        air += b.bp * b.k * nCur[i];               // k * bp: unit gain at the centre
      }
      let arm = 0;
      if (nModes) {
        const e = seekSample();
        if (armLive) {
          for (let i = 0; i < nModes; i++) {
            const m = modes[i], m2 = modes2[i];
            runSvf(m, e); runSvf(m2, m.bp * m.k);
            arm += m2.bp * m2.k * mGain[i];
          }
          arm *= seekAmp;
          work.stages += 2 * nModes;
        }
      } else if (act) seekSample();                 // no modes: the head still moves
      // An equal-power crossfade, so a half-and-half mix isn't quieter than either.
      if (grainMix > 0) arm = arm * gSynthGain + grainSample() * gGrainGain;
      let y = (tones * wob + air + arm) * master;
      y = clamp(y, -1, 1);
      out[j] = y;
      const ay = y < 0 ? -y : y; if (ay > peak) peak = ay;
      sq += y * y; n++;
    }
  }

  function telemetry() {
    const t = { rpm, state, powered, fr: frNow, peak, rms: Math.sqrt(sq / Math.max(1, n)), head: headX, seeking: !!sk, busy, script: scr < 0 ? null : scr ? 'spindown' : 'spinup' };
    peak = 0; sq = 0; n = 0;
    return t;
  }

  // The work done since the last call (for bench.js's cost estimate).
  function workDone() { const w = { ...work }; for (const k in work) work[k] = 0; return w; }

  return { setParams, handle, process, telemetry, workDone, body, NOISE_HZ: Array.from(noiseHz), NOISE_Q, MAX_TONES, MAX_BODY, MAX_CURVE, MAX_MODES, MAX_EVENTS };
}

if (typeof module !== 'undefined') module.exports = { makeEngine };
