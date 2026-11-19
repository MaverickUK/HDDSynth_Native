// Designing a drive from scratch: plain choices about the hardware (its size and speed,
// the motor, the bearing, the arm, how it parks, how it's mounted) turned into the
// engine's detailed model (tones, body, air noise, arm modes, head events).
//
// The mapping follows the physics where there is some:
//   - the motor's tones: the commutation (6 per pole pair per turn, for a 3-phase
//     motor), the slots, and the cogging between them (lcm of slots and poles)
//   - a ball bearing's defect tones, from its geometry (cage, ball spin, outer and
//     inner race), louder with wear. A fluid bearing has none of these, only a
//     faint whirl at just under half the rotation rate.
//   - the body: three resonances, lower for a bigger drive and more platters, shaped
//     by the mounting (a bare drive on a desk, in a PC case, on a rubber sled)
//   - the air: noise peaking higher and louder with speed, louder with more platters
//   - the arm: a rock lower for a bigger, heavier arm, and its overtones
//   - head events: the motor's start-up kick, loading the heads (off the ramp, or off
//     the landing zone), calibration seeks, and the park at power off
// Then it renders a little and sets the levels: idle at about -20 dBFS (as recorded
// packs are), seeks peaking about -4 dBFS.
//
// The choices are kept in the drive (drive.design), so a designed drive reopens with
// them. Editing the detailed model by hand is allowed, until a choice changes.
const { renderIdle, renderHeads } = require('./render.js');

const DESIGN_DEFAULT = {
  size: '3.5', platters: 2, rpm: 7200, spinupS: 0,       // 0: from the size and speed
  motorPoles: 8, motorSlots: 12, whine: 0.5, imbalance: 0.3,
  bearing: 'ball', wear: 0.2,
  actuator: 'voice', seekMs: 18, stepHz: 250, seekDb: 0,
  park: 'ramp', calibration: 3,
  mount: 'case', bright: 0, seed: 1,
};

// Per size: body resonances, the arm's rock, spin-up time at 5400 rpm, the bearing's
// balls, where the air noise peaks (Hz at 5400 rpm) and how loud it is.
const SIZES = {
  '5.25': { body: [[240, 3, 9], [760, 5, 7], [1900, 5, 5]], armHz: 210, spinupS: 9, balls: 9, airHz: 650, airDb: 3, steps: 600 },
  '3.5': { body: [[420, 4, 9], [1350, 6, 7], [3100, 5, 5]], armHz: 330, spinupS: 5.5, balls: 8, airHz: 1000, airDb: 0, steps: 400 },
  '2.5': { body: [[700, 4, 8], [2300, 6, 6], [5200, 5, 5]], armHz: 560, spinupS: 3, balls: 7, airHz: 1500, airDb: -6, steps: 300 },
};

const PRESETS = [
  { id: 'stepper86', name: '1986 full-height stepper', note: '5.25″, 3600 rpm, a stepper motor moving the heads, no parking',
    design: { size: '5.25', platters: 4, rpm: 3600, motorPoles: 8, motorSlots: 12, whine: 0.7, imbalance: 0.5, bearing: 'ball', wear: 0.5, actuator: 'stepper', stepHz: 200, park: 'none', calibration: 1, mount: 'case', bright: -0.2, seed: 86 } },
  { id: 'desk91', name: '1991 desktop', note: '3.5″, 3600 rpm, an early voice coil landing its heads on the disk',
    design: { size: '3.5', platters: 2, rpm: 3600, motorPoles: 8, motorSlots: 9, whine: 0.6, imbalance: 0.4, bearing: 'ball', wear: 0.35, actuator: 'voice', seekMs: 38, park: 'zone', calibration: 2, mount: 'case', bright: -0.1, seed: 91 } },
  { id: 'desk97', name: '1997 desktop', note: '3.5″, 5400 rpm, ball bearings, landing zone',
    design: { size: '3.5', platters: 3, rpm: 5400, motorPoles: 8, motorSlots: 12, whine: 0.45, imbalance: 0.25, bearing: 'ball', wear: 0.2, actuator: 'voice', seekMs: 24, park: 'zone', calibration: 4, mount: 'case', bright: 0, seed: 97 } },
  { id: 'desk03', name: '2003 desktop', note: '3.5″, 7200 rpm, fluid bearing, ramp load',
    design: { size: '3.5', platters: 2, rpm: 7200, motorPoles: 8, motorSlots: 12, whine: 0.3, imbalance: 0.15, bearing: 'fdb', wear: 0.1, actuator: 'voice', seekMs: 18, park: 'ramp', calibration: 3, mount: 'case', bright: 0.1, seed: 3 } },
  { id: 'ent06', name: '2006 enterprise', note: '3.5″, 10,000 rpm, fast seeks, a lot of air noise',
    design: { size: '3.5', platters: 3, rpm: 10000, motorPoles: 8, motorSlots: 12, whine: 0.4, imbalance: 0.1, bearing: 'fdb', wear: 0.1, actuator: 'voice', seekMs: 10, park: 'ramp', calibration: 5, mount: 'sled', bright: 0.3, seed: 6 } },
  { id: 'laptop08', name: '2008 laptop', note: '2.5″, 5400 rpm, one platter, very quiet',
    design: { size: '2.5', platters: 1, rpm: 5400, motorPoles: 8, motorSlots: 9, whine: 0.25, imbalance: 0.1, bearing: 'fdb', wear: 0.05, actuator: 'voice', seekMs: 22, park: 'ramp', calibration: 2, mount: 'bare', bright: 0.2, seed: 8 } },
];

function seeded(seed) {
  let s = (seed || 1) * 2654435761 >>> 0 || 1;
  return () => { s ^= s << 13; s ^= s >>> 17; s ^= s << 5; return (s >>> 0) / 4294967296; };
}
const gcd = (a, b) => (b ? gcd(b, a % b) : a);
const lcm = (a, b) => a / gcd(a, b) * b;
const r1 = (v) => Math.round(v * 10) / 10, r3 = (v) => Math.round(v * 1000) / 1000;
const NOISE_HZ = Array.from({ length: 18 }, (_, i) => 40 * Math.pow(2, i / 2));

// How long it takes to spin up, if not given: bigger, faster and more platters take longer.
function spinupFor(s) {
  const S = SIZES[s.size] || SIZES['3.5'];
  return s.spinupS > 0 ? s.spinupS : r1(S.spinupS * Math.sqrt(s.rpm / 5400) * (1 + 0.08 * (s.platters - 2)));
}

function designToDrive(spec) {
  const s = { ...DESIGN_DEFAULT, ...(spec || {}) };
  const S = SIZES[s.size] || SIZES['3.5'];
  const rnd = seeded(s.seed);
  const vary = (db) => r1(db + (rnd() - 0.5) * 4);          // +-2 dB, so no two seeds sound alike
  const ball = s.bearing !== 'fdb';

  // ---- tones (orders of the rotation rate) ----
  const tones = [];
  const add = (order, db, motor) => { if (order > 0.2 && db > -90) tones.push(motor ? { order: r3(order), db: vary(db), motor: true } : { order: r3(order), db: vary(db) }); };
  add(1, -30 + 10 * s.imbalance);                            // the platters' imbalance: once a turn
  add(2, -40 + 8 * s.imbalance);
  add(3, -48 + 4 * s.imbalance);
  const pp = Math.max(1, Math.round(s.motorPoles / 2));
  add(6 * pp, -36 + 12 * s.whine, true);                     // commutation: the motor's whine
  add(12 * pp, -44 + 12 * s.whine, true);
  add(18 * pp, -52 + 10 * s.whine, true);
  add(s.motorSlots, -42 + 8 * s.whine);                       // slots passing the magnets
  const cog = lcm(s.motorSlots, s.motorPoles);
  if (cog <= 120) add(cog, -48 + 8 * s.whine);
  if (ball) {
    // Ball-bearing defect frequencies (orders), for balls of a quarter the pitch diameter.
    const n = S.balls, r = 0.25, ftf = 0.5 * (1 - r), bsf = (1 - r * r) / (2 * r);
    const w = -54 + 16 * s.wear;
    add(ftf, w - 2); add(bsf, w); add(2 * bsf, w - 4);
    add(n * ftf, w + 2); add(n * (1 - ftf), w); add(2 * n * ftf, w - 4); add(2 * n * (1 - ftf), w - 6);
  } else {
    add(0.47, -62 + 10 * s.wear);                             // oil whirl, just under half
  }
  tones.sort((a, b) => a.order - b.order);

  // ---- body ----
  const pScale = Math.pow(0.97, s.platters - 2);
  const body = S.body.map(([hz, q, db]) => ({ hz: Math.round(hz * pScale), q, db }));
  if (s.mount === 'bare') { body[0].db -= 6; body[2].db += 2; }
  else if (s.mount === 'case') { body[0].db += 3; body.unshift({ hz: 140, q: 2, db: 6 }); }    // the case booms
  else if (s.mount === 'sled') body.forEach((b) => { b.db = r1(b.db * 0.6); b.q = r1(b.q * 0.7); });
  body[body.length - 1].db = r1(body[body.length - 1].db + 5 * s.bright);

  // ---- air noise (half-octave bands, 40 Hz up) ----
  const fc = S.airHz * Math.pow(s.rpm / 5400, 0.8);
  const L0 = -50 + S.airDb + 4 * Math.log2(Math.max(1, s.platters) / 2) + 25 * Math.log10(s.rpm / 5400) - (ball ? 0 : 2);
  const noiseDb = NOISE_HZ.map((f) => {
    const o = Math.log2(f / fc);
    let v = L0 - 2.2 * o * o + 2 * s.bright * o;
    if (ball && f < 300) v += 6 * s.wear;                      // a worn bearing rumbles
    return r1(Math.max(-90, v));
  });

  // ---- spindle ----
  const spinupS = spinupFor(s);
  const coastS = r1(spinupS * (ball ? 1.3 : 1.7) * (1 + 0.1 * (s.platters - 2)));
  const spindle = {
    rpm: s.rpm, spinupS, coastS, friction: ball ? 0.15 : 0.06, speedExp: 2,
    motorBoostDb: r1(4 + 6 * s.whine), jitter: 0.0005, wobble: r3(0.1 * s.imbalance),
    flutterDb: s.mount === 'bare' ? 2.5 : s.mount === 'sled' ? 1.5 : 2, flutterHz: 2,
  };

  // ---- actuator ----
  const armHz = S.armHz * Math.pow(0.95, s.platters - 2);
  const nyq = 7000;
  let actuator, workload;
  if (s.actuator === 'stepper') {
    actuator = {
      kind: 'stepper', stepHz: s.stepHz, strokeSteps: S.steps, settleMs: 4, levelExp: 0.3,
      clickDb: 0, thumpDb: -4, buzzDb: -12, db: 0,
      modes: [[Math.max(150, s.stepHz), 4, 8], [armHz * 2, 3, 2], [2200, 4, -6]].filter((m) => m[0] < nyq).map(([hz, q, db]) => ({ hz: Math.round(hz), q, db })),
    };
    workload = { gapMs: 30, locality: 0.3, seqShare: 0.1, seqRevs: 2 };
  } else {
    actuator = {
      kind: 'voice', trackMs: r1(Math.max(0.8, s.seekMs / 9)), fullStrokeMs: s.seekMs, settleMs: 3, levelExp: 0.5,
      clickDb: -8, thumpDb: 0, buzzDb: -6, db: 0,
      modes: [[armHz, 2.5, 10], [armHz * 1.6, 3, 2], [armHz * 4.5, 5, -4], [armHz * 8, 5, -8]].filter((m) => m[0] < nyq).map(([hz, q, db]) => ({ hz: Math.round(hz), q, db })),
    };
    workload = { gapMs: r1(4 + 60000 / s.rpm / 4), locality: 0.2, seqShare: 0.1, seqRevs: 2 };
  }

  const drive = {
    version: 3,
    name: spec && spec.name ? spec.name : 'Designed drive',
    note: 'Designed in the Drive Lab',
    design: { ...s },
    spindle, tones,
    bed: { count: ball ? 26 : 20, db: ball ? -58 + 4 * s.wear : -62, maxOrder: 90, fractional: ball ? 0.4 : 0.2, seed: s.seed },
    body, noise: { db: noiseDb, speedExp: 2.5, follow: 0.5 },
    actuator, workload,
    heads: headsFor(s, spindle, actuator),
    master: 1,
  };
  delete drive.design.name;
  return drive;
}

// Where the head rests with the power off: on the ramp just off the platter's edge,
// on the landing zone at the inner edge, or wherever it was.
const restX = (park) => (park === 'ramp' ? -0.06 : park === 'zone' ? 1 : 0.1);

function headsFor(s, spindle, act) {
  const up = [], down = [];
  const ev = (list, t, ms, db, x) => { const e = { t: r3(t), ms: r1(ms), db: r1(db) }; if (x != null) e.x = x; list.push(e); };
  const stepper = act.kind === 'stepper';
  const stroke = stepper ? act.strokeSteps / act.stepHz * 1000 : act.fullStrokeMs;
  // The motor's start-up kick (heads stuck to the landing zone kick harder: stiction).
  ev(up, 0.05, 60, s.park === 'zone' ? -10 : -14);
  ev(up, 0.3, 40, -18);
  // Old, slow-starting motors tick as they find their speed.
  if (stepper || s.size === '5.25') {
    for (let t = 0.45, dt = 0.1; t < 1.6; t += dt, dt = Math.max(0.025, dt * 0.85)) ev(up, t, 1, -22);
  }
  // Loading the heads, once up to speed.
  let t = spindle.spinupS * 0.95 + 0.25;
  if (s.park === 'ramp') { ev(up, t, 30, -8, 0.06); t += 0.2; }
  else if (s.park === 'zone') { ev(up, t, 1, 0, 0.97); ev(up, t + 0.05, stroke, -3, 0.05); t += 0.25; }
  // Calibration: full strokes back and forth, then a sweep of short steps, then a few seeks.
  if (stepper) {
    if (s.calibration > 0) { ev(up, t, stroke * 0.6, -2, 0); t += stroke * 0.6 / 1000 + 0.1; ev(up, t, stroke * 0.2, -4, 0.2); t += 0.15; }
    for (let i = 1; i < s.calibration; i++) { ev(up, t, stroke * 0.1, -6); t += 0.12; }
  } else if (s.calibration > 0) {
    for (let i = 0; i < s.calibration; i++) { ev(up, t, stroke, -3, i % 2 ? 0.05 : 1); t += 0.16; }
    for (let i = 0; i < 8; i++) { ev(up, t, 2, -4); t += 0.03; }
    ev(up, t + 0.12, stroke * 0.65, -2, 0.6); ev(up, t + 0.27, stroke * 0.65, -2, 0.2); ev(up, t + 0.5, stroke * 0.55, -3, 0.5);
  }
  // The park at power off, then the heads landing as it slows, and the stop.
  if (!stepper && s.park !== 'none') {
    const x = restX(s.park);
    ev(down, 0, 22, -3, x);
    ev(down, 0.03, 1, 4, x);                                   // the latch
    if (s.park === 'zone') ev(down, spindle.coastS * 0.75, 80, -22, x);   // landing: the heads skid
    ev(down, spindle.coastS * 0.98, 4, -12, x);
  }
  return { rest: restX(stepper ? 'none' : s.park), spinup: up, spindown: down };
}

// Sets the drive's levels by rendering it: idle RMS to idleDb, the loudest seeks to
// seekDb (plus the design's seek loudness).
function levelDrive(drive, opts) {
  opts = opts || {};
  const sr = opts.sr || 16000, idleDb = opts.idleDb == null ? -20 : opts.idleDb, seekDb = (opts.seekDb == null ? -4 : opts.seekDb) + ((drive.design && drive.design.seekDb) || 0);
  const rmsDb = (x) => { let s = 0; for (const v of x) s += v * v; return 10 * Math.log10(s / x.length + 1e-20); };
  const idle = renderIdle(drive, sr, 1.5);
  drive.master = r3(drive.master * Math.pow(10, (idleDb - rmsDb(idle)) / 20));
  const peakDb = (x) => { let m = 0; for (const v of x) m = Math.max(m, Math.abs(v)); return 20 * Math.log10(m + 1e-9); };
  if (drive.actuator) {
    // Seeks: the loudest is a full stroke, so render one (alone, over the idle's level).
    const one = { ...drive, heads: { rest: 0, spinup: [{ t: 0.05, ms: drive.actuator.kind === 'stepper' ? drive.actuator.strokeSteps / drive.actuator.stepHz * 1000 : drive.actuator.fullStrokeMs, db: 0, x: 1 }] } };
    const full = peakDb(renderHeads(one, sr, 'spinup', 1)) + 20 * Math.log10(drive.master);
    drive.actuator.db = r1(drive.actuator.db + Math.max(-20, Math.min(20, seekDb - full)));
    // Head events: none louder than -3 dBFS (their levels are against a seek's).
    for (const k of ['spinup', 'spindown']) {
      const ev = drive.heads && drive.heads[k];
      if (!ev || !ev.length) continue;
      const secs = Math.max(...ev.map((h) => h.t)) + 0.5;
      const over = peakDb(renderHeads(drive, sr, k, secs)) + 20 * Math.log10(drive.master) + 3;
      if (over > 0) ev.forEach((h) => { h.db = r1(h.db - over); });
    }
  }
  return drive;
}

function designDrive(spec, opts) { return levelDrive(designToDrive(spec), opts); }

module.exports = { designDrive, designToDrive, levelDrive, DESIGN_DEFAULT, PRESETS, SIZES, spinupFor };
