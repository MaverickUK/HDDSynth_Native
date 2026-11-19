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
  let nBody = 0;
  const bHz = new Float64Array(MAX_BODY), bQ = new Float64Array(MAX_BODY), bGain = new Float64Array(MAX_BODY);
  const nRms = new Float64Array(NB);   // each noise band's RMS at full speed

  // Body response at f: 1 plus a resonance curve per mode (linear amplitude).
  function body(f) {
    let h = 1;
    for (let i = 0; i < nBody; i++) {
      const r = f / bHz[i] - bHz[i] / f;
      h += (bGain[i] - 1) / (1 + bQ[i] * bQ[i] * r * r);
    }
    return h > 0 ? h : 0;
  }

  // ---- state ----
  let powered = false, state = 'off';           // off | spinup | ready | coast
  let rpm = 0, accel = 0, wander = 0, frNow = 0;
  let phRot = 0;
  const tPh = new Float64Array(MAX_TONES), tCur = new Float64Array(MAX_TONES), tStep = new Float64Array(MAX_TONES);
  const bands = []; for (let i = 0; i < NB; i++) bands.push(svf());
  const nCur = new Float64Array(NB), nStep = new Float64Array(NB);
  let ctrlCount = 0;

  function setParams(drive) {
    d = drive;
    const tones = drive.tones || [];
    nTones = Math.min(tones.length, MAX_TONES);
    for (let i = 0; i < nTones; i++) {
      tOrder[i] = tones[i].order; tAmp[i] = dbToAmp(tones[i].db); tMotor[i] = tones[i].motor ? 1 : 0;
      tPh[i] = rnd();                                 // random start phases: no click of aligned sines
    }
    const b = drive.body || [];
    nBody = Math.min(b.length, MAX_BODY);
    for (let i = 0; i < nBody; i++) { bHz[i] = b[i].hz; bQ[i] = b[i].q; bGain[i] = dbToAmp(b[i].db); }
    const nb = (drive.noise && drive.noise.db) || [];
    for (let i = 0; i < NB; i++) nRms[i] = i < nb.length ? dbToAmp(nb[i]) : 0;
  }

  // Runs at CTRL_HZ: the spindle speed, then every tone's and band's target level.
  function control() {
    const s = d.spindle, target = s.rpm;
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
      const tau = s.coastS / 2.5;
      rpm -= ctrlDt * (rpm / tau + 0.2236 * target / s.coastS);
      if (rpm <= 0) { rpm = 0; state = 'off'; }
      accel *= 0.97;
    }
    // Slow random wander of the speed: no motor holds it perfectly steady.
    wander = wander * 0.995 + noise() * 0.1;
    frNow = (rpm / 60) * (1 + (s.jitter || 0) * wander);

    const sn = target > 0 ? rpm / target : 0;
    const mech = Math.pow(sn, s.speedExp == null ? 2 : s.speedExp);
    // Motor tones follow the drive current: louder while accelerating, gone once unpowered.
    const motor = powered ? (1 + (dbToAmp(s.motorBoostDb || 0) - 1) * accel) * Math.min(1, sn * 10) : 0;
    const nyq = sr * 0.45, fade0 = sr * 0.4;
    for (let i = 0; i < nTones; i++) {
      const f = tOrder[i] * frNow;
      let a = 0;
      if (f > 1 && f < nyq) {
        a = tAmp[i] * (tMotor[i] ? motor : mech) * body(f);
        if (f > fade0) a *= (nyq - f) / (nyq - fade0);
      }
      tStep[i] = (a - tCur[i]) / ctrlEvery;
    }
    const nz = d.noise || {};
    const nLaw = Math.pow(sn, nz.speedExp == null ? 2.5 : nz.speedExp);
    const shift = Math.pow(Math.max(sn, 0.05), nz.follow == null ? 0.5 : nz.follow);
    for (let i = 0; i < NB; i++) {
      const fc = noiseHz[i] * shift;
      let g = 0;
      if (fc < nyq && nRms[i] > 0) {
        tuneSvf(bands[i], fc, NOISE_Q);
        // A unit-peak band-pass passes pi*fc/(Q*sr) of unit white noise's power.
        g = nRms[i] * nLaw * body(fc) / Math.sqrt(Math.PI * fc / (NOISE_Q * sr));
      }
      nStep[i] = (g - nCur[i]) / ctrlEvery;
    }
  }

  function power(on) {
    if (on === powered) return;
    powered = on;
    state = on ? 'spinup' : 'coast';
  }

  function handle(m) {
    if (m.type === 'params') setParams(m.drive);
    else if (m.type === 'power') power(m.on);
    else if (m.type === 'ready') { powered = true; rpm = d.spindle.rpm; accel = 0; state = 'ready'; } // jump to full speed (renders, tests)
  }

  let peak = 0, sq = 0, n = 0;
  function process(out, len) {
    for (let j = 0; j < len; j++) {
      if (++ctrlCount >= ctrlEvery) { ctrlCount = 0; control(); }
      phRot += frNow * dt; if (phRot >= 1) phRot -= 1;
      const wob = 1 + (d.spindle.wobble || 0) * 0.5 * Math.sin(TAU * phRot);

      let tones = 0;
      for (let i = 0; i < nTones; i++) {
        tPh[i] += tOrder[i] * frNow * dt; tPh[i] -= Math.floor(tPh[i]);
        tCur[i] += tStep[i];
        tones += tCur[i] * Math.sin(TAU * tPh[i]);
      }
      let air = 0;
      for (let i = 0; i < NB; i++) {
        nCur[i] += nStep[i];
        if (nCur[i] === 0 && nStep[i] === 0) continue;
        const b = bands[i];
        runSvf(b, noise());
        air += b.bp * b.k * nCur[i];               // k * bp: unit gain at the centre
      }
      let y = (tones * wob + air) * (d.master == null ? 1 : d.master);
      y = clamp(y, -1, 1);
      out[j] = y;
      const ay = y < 0 ? -y : y; if (ay > peak) peak = ay;
      sq += y * y; n++;
    }
  }

  function telemetry() {
    const t = { rpm, state, powered, fr: frNow, peak, rms: Math.sqrt(sq / Math.max(1, n)) };
    peak = 0; sq = 0; n = 0;
    return t;
  }

  return { setParams, handle, process, telemetry, NOISE_HZ: Array.from(noiseHz), MAX_TONES, MAX_BODY };
}

if (typeof module !== 'undefined') module.exports = { makeEngine };
