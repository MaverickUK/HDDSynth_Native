// HDD Synth Drive Lab: the drive model and synthesis engine.
// makeEngine() is self-contained (no outer references) so its source can be sent
// to an AudioWorklet with Function.toString(). Written in plain, fixed-structure
// code that maps onto C++ for the RP2350 later.

function makeEngine(sr) {
  const TAU = Math.PI * 2, dt = 1 / sr;
  const CTRL_HZ = 1000, ctrlEvery = Math.round(sr / CTRL_HZ), ctrlDt = ctrlEvery / sr;

  // ---- small DSP helpers ----
  let seed = 0x2545F491;
  const rnd = () => { seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5; return (seed >>> 0) / 4294967296; };
  const noise = () => rnd() * 2 - 1;
  const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

  function biquad() { return { b0: 1, b1: 0, b2: 0, a1: 0, a2: 0, z1: 0, z2: 0 }; }
  function setBiquad(f, type, freq, q, db) {
    const A = Math.pow(10, db / 40), w = TAU * Math.min(freq, sr * 0.45) / sr;
    const cw = Math.cos(w), sw = Math.sin(w), al = sw / (2 * q);
    let b0, b1, b2, a0, a1, a2;
    if (type === 'peak') {
      b0 = 1 + al * A; b1 = -2 * cw; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * cw; a2 = 1 - al / A;
    } else {
      const s = 2 * Math.sqrt(A) * al;
      if (type === 'low') {
        b0 = A * ((A + 1) - (A - 1) * cw + s); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - s);
        a0 = (A + 1) + (A - 1) * cw + s; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - s;
      } else {
        b0 = A * ((A + 1) + (A - 1) * cw + s); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - s);
        a0 = (A + 1) - (A - 1) * cw + s; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - s;
      }
    }
    f.b0 = b0 / a0; f.b1 = b1 / a0; f.b2 = b2 / a0; f.a1 = a1 / a0; f.a2 = a2 / a0;
  }
  function runBiquad(f, x) {
    const y = f.b0 * x + f.z1;
    f.z1 = f.b1 * x - f.a1 * y + f.z2;
    f.z2 = f.b2 * x - f.a2 * y;
    return y;
  }

  // State-variable filter (TPT form): stays stable while its cutoff moves.
  function svf() { return { a1: 0, a2: 0, a3: 0, k: 1, ic1: 0, ic2: 0, lp: 0, bp: 0 }; }
  function tuneSvf(s, fc, q) {
    const g = Math.tan(Math.PI * clamp(fc, 10, sr * 0.45) / sr);
    s.k = 1 / q; s.a1 = 1 / (1 + g * (g + s.k)); s.a2 = g * s.a1; s.a3 = g * s.a2;
  }
  function runSvf(s, x) {
    const v3 = x - s.ic2, v1 = s.a1 * s.ic1 + s.a2 * v3, v2 = s.ic2 + s.a2 * s.ic1 + s.a3 * v3;
    s.ic1 = 2 * v1 - s.ic1; s.ic2 = 2 * v2 - s.ic2; s.lp = v2; s.bp = v1;
  }

  // ---- parameters ----
  let p = {};

  // ---- modal bank: the arm and the case ringing when the actuator kicks them ----
  const ARM = [1, 1.62, 2.31, 2.97, 3.83, 4.7, 6.1, 7.9];
  const CASE = [1, 1.47, 2.09, 2.83];
  const NM = ARM.length + CASE.length;
  const mc1 = new Float64Array(NM), mc2 = new Float64Array(NM), mg = new Float64Array(NM);
  const my1 = new Float64Array(NM), my2 = new Float64Array(NM);
  function tuneModes() {
    for (let i = 0; i < NM; i++) {
      const isArm = i < ARM.length, j = isArm ? i : i - ARM.length;
      const f = isArm ? p.armHz * ARM[j] : p.caseHz * CASE[j];
      if (f > sr * 0.42) { mg[i] = 0; mc1[i] = 0; mc2[i] = 0; continue; }
      const t60 = isArm ? (p.armRingMs / 1000) / (1 + j * 0.25) : (p.caseRingMs / 1000) / (1 + j * 0.3);
      const amp = isArm ? 1 / (1 + 0.35 * j) : 0.9 / (1 + 0.5 * j);
      const r = Math.exp(-6.91 / (t60 * sr)), w = TAU * f / sr;
      mc1[i] = 2 * r * Math.cos(w); mc2[i] = -r * r; mg[i] = amp * Math.sin(w);
    }
  }

  // ---- enclosure EQ ----
  const eqLow = biquad(), eqCase = biquad(), eqHigh = biquad();
  function tuneEq() {
    const lowDb = p.mount === 'bare' ? -5 : p.mount === 'sled' ? 1 : 4;
    setBiquad(eqLow, 'low', 220, 0.707, lowDb);
    setBiquad(eqCase, 'peak', p.caseHz * 0.8, 1.6, p.caseRes * 9 * (p.mount === 'bare' ? 0.4 : 1));
    setBiquad(eqHigh, 'high', 3500, 0.707, p.bright * 9 - (p.mount === 'case' ? 2 : 0));
  }

  // ---- noise filters (retuned at the control rate as the spindle speed changes) ----
  const windLp = svf(), windBp = svf(), bearBp = svf(), servoLp = svf(), scrapeBp = svf();
  tuneSvf(servoLp, 5000, 0.7);

  // ---- state ----
  let t = 0, ctrlCount = 0;
  let powered = false, state = 'off';          // off | spinup | calib | ready
  let rpm = 0, motorCurrent = 0;
  let phRot = 0, phWhine = 0, phElec = 0, phBear = 0, phServo = 0;
  let windAmp = 0, bearAmp = 0, bearToneAmp = 0, scrapeAmp = 0, servoGate = 0;
  let humAmp = 0, whineAmp = 0, elecAmp = 0;
  let workload = 'idle', flood = false, busy = false;

  // Head. x is the position across the stroke: 0 = outer edge (LBA 0), 1 = inner edge.
  let x = 0, v = 0, xt = 0, aPrev = 0, vcMode = 1; // 0 = seek, 1 = follow (track following / settling)
  let A = 1e4, w = 4000, zeta = 0.5, retract = false;
  const SETTLE_WIN = 0.003;
  // Stepper
  let track = 0, tTrack = 0, stepTimer = 0, stepsDone = 0, stSettle = 0, xPrev = 0;
  // Excitation for the modal bank
  let excLp = 0, excK = 0.5, clickAcc = 0, caseAcc = 0;
  // Bookkeeping
  let settledMs = 0, seekPending = false, seekT0 = 0, seekD = 0, lastSeekMs = 0, lastSeekD = 0, seekCount = 0;
  let parkPending = false;

  const parkX = () => (p.park === 'ramp' ? -0.06 : p.park === 'zone' ? 1.04 : x);
  const isStepper = () => p.actuator === 'stepper';
  const RI = 0.45; // inner/outer radius of the data zone
  function mapX(f) {
    f = clamp(f, 0, 1);
    if (!p.zoned) return f;
    // Zoned recording: constant areal density, so LBA fraction f covers the area outside radius r.
    const r = Math.sqrt(1 - f * (1 - RI * RI));
    return (1 - r) / (1 - RI);
  }

  function click(amount, caseAmount) { clickAcc += amount; caseAcc += caseAmount || 0; }
  const clickAmp = (d) => 0.15 + 0.85 * Math.sqrt(Math.min(1, d / 0.25));

  function seekTo(xs, tick) {
    if (isStepper()) {
      const cyl = Math.max(2, p.cylinders | 0);
      const nt = clamp(Math.round(clamp(xs, 0, 1) * (cyl - 1)), 0, cyl - 1);
      if (nt === tTrack) return;
      seekD = Math.abs(nt - track) / (cyl - 1);
      tTrack = nt; stepsDone = 0;
    } else {
      const d = Math.abs(xs - x);
      xt = xs; seekD = d;
      if (d > SETTLE_WIN) { vcMode = 0; click(clickAmp(d) * 0.6, 0); }
      else click(clickAmp(d) * (tick == null ? 1 : tick), 0);
    }
    seekPending = true; seekT0 = t;
  }

  function headSettled() {
    if (isStepper()) return track === tTrack && stepTimer <= 0 && stSettle <= 0;
    return vcMode === 1 && settledMs >= 1;
  }

  // ---- scripts (power-on calibration, thermal recalibration) and workload jobs ----
  let script = [], scriptWait = 0;
  let jobs = [], job = null, jobT = 0, jobStarted = false, jobLat = 0, retX = 0, seqAcc = 0;

  function calibrationScript() {
    const s = [];
    if (isStepper()) {
      s.push({ x: 0 });                          // recalibrate: step out to track 0
      s.push({ wait: 0.05 });
    } else if (p.park !== 'none') {
      s.push({ click: 0.5, caseKick: 0.9 });     // latch release
      s.push({ wait: 0.03 });
      s.push({ x: p.park === 'ramp' ? 0.01 : 0.96 }); // unload from the ramp / lift off the landing zone
      s.push({ wait: 0.04 });
    }
    const spots = [0.98, 0.02, 0.5, 0.25, 0.75, 0.1, 0.9, 0.4];
    for (let i = 0; i < (p.calSeeks | 0); i++) {
      s.push({ x: spots[i % spots.length] });
      s.push({ wait: 0.03 + 0.05 * rnd() });
    }
    s.push({ x: 0.01 });
    return s;
  }

  function recalScript() {
    return [{ x: 0.95 }, { wait: 0.02 }, { x: 0.05 }, { wait: 0.02 }, { x: 0.6 }, { wait: 0.03 }, { x: 0.3 }];
  }

  function refillJobs() {
    switch (workload) {
      case 'random': jobs.push({ k: 'seek', f: rnd() }); break;
      case 'seq': jobs.push({ k: 'seq', ms: 300 + rnd() * 400 }, { k: 'seek', f: 0.002, save: true }, { k: 'seq', ms: 10 }, { k: 'seek', ret: true }); break;
      case 'boot':
        jobs.push({ k: 'seek', f: 0.001 }, { k: 'seq', ms: 8 }, { k: 'seek', f: 0.002 + rnd() * 0.3 },
          { k: 'seq', ms: 40 + rnd() * 260 }, { k: 'pause', ms: rnd() < 0.3 ? 200 + rnd() * 500 : rnd() * 60 });
        break;
      case 'defrag':
        jobs.push({ k: 'seek', f: 0.4 + rnd() * 0.55 }, { k: 'seq', ms: 120 + rnd() * 80 },
          { k: 'seek', f: 0.03 + rnd() * 0.2 }, { k: 'seq', ms: 120 + rnd() * 80 }, { k: 'seek', f: 0.001 }, { k: 'seq', ms: 6 });
        break;
      default: jobs.push({ k: 'pause', ms: 50 });
    }
  }

  let recalTimer = 0;

  function control() {
    // Spindle speed: motor torque up, windage and friction down.
    if (powered) {
      if (rpm < p.rpm) {
        rpm += (p.rpm / p.spinupS) * ctrlDt * (1.25 - 0.5 * rpm / p.rpm);
        motorCurrent += (1 - motorCurrent) * 0.05;
        if (rpm > p.rpm) rpm = p.rpm;
      } else {
        rpm += (p.rpm - rpm) * 0.02;
        motorCurrent += (0.15 - motorCurrent) * 0.01;
      }
    } else {
      // Windage (proportional to speed) plus bearing friction (constant): stops at exactly coastS.
      const tau = p.coastS / 2.5;
      rpm -= ctrlDt * (rpm / tau + 0.2236 * p.rpm / p.coastS);
      if (rpm < 0) rpm = 0;
      motorCurrent *= 0.97;
    }
    const sn = p.rpm > 0 ? rpm / p.rpm : 0, sa = rpm / 7200;
    const sizeF = p.size === '5.25' ? 1.5 : p.size === '2.5' ? 0.7 : 1;
    const ball = p.bearing === 'ball';

    humAmp = p.hum * 0.05 * Math.min(1, sn * 3);
    whineAmp = p.whine * 0.035 * (0.3 + 1.7 * motorCurrent) * Math.min(1, sn * 4);
    elecAmp = p.whine * 0.012 * motorCurrent * Math.min(1, sn * 6);
    windAmp = p.windage * 0.22 * Math.sqrt(p.platters / 2) * sizeF * sa * sa;
    tuneSvf(windLp, 120 + 2600 * sa * sizeF, 0.6);
    tuneSvf(windBp, 250 + 1200 * sa * sizeF, 1.3);
    bearAmp = p.bearingLvl * (ball ? 0.05 : 0.012) * (0.3 + p.wear) * Math.pow(sn, 1.5);
    bearToneAmp = p.bearingLvl * (ball ? 0.012 : 0.003) * p.wear * sn;
    tuneSvf(bearBp, (ball ? 2400 : 1600) * (0.4 + 0.6 * sn) * (1 + 0.3 * p.wear), ball ? 2.5 : 1.2);
    // Contact start-stop heads touching down as the platters slow.
    const landing = p.park === 'zone' && !isStepper() && !powered && sn > 0.02 && sn < 0.35;
    scrapeAmp = landing ? 0.05 * (1 - sn / 0.35) * p.bearingLvl : 0;
    tuneSvf(scrapeBp, 900 + 3000 * sn, 1.5);

    // Head settled?
    if (!isStepper()) {
      if (vcMode === 1 && Math.abs(x - xt) < 0.0006 && Math.abs(v) < 0.08) settledMs++; else settledMs = 0;
    } else if (stSettle > 0) stSettle -= ctrlDt;
    if (seekPending && headSettled()) {
      seekPending = false; lastSeekMs = (t - seekT0) * 1000; lastSeekD = seekD; seekCount++;
      if (parkPending) { parkPending = false; retract = false; if (!isStepper()) click(0.7, 1.0); } // latch clack
    }

    // Power sequencing
    if (state === 'spinup' && rpm >= p.rpm * 0.97) { state = 'calib'; script = calibrationScript(); scriptWait = 0; }
    if (script.length) {
      if (scriptWait > 0) scriptWait -= ctrlDt;
      else if (headSettled() || script[0].x === undefined) {
        const s = script.shift();
        if (s.x !== undefined) seekTo(s.x);
        if (s.click) click(s.click, s.caseKick || 0);
        if (s.wait) scriptWait = s.wait;
      }
    } else if (state === 'calib') { state = 'ready'; job = null; jobs.length = 0; }

    busy = false;
    if (state === 'ready' && !script.length) {
      if (workload === 'idle' || (job && job.k === 'pause')) {
        recalTimer += ctrlDt;
        if (p.recalS > 0 && recalTimer > p.recalS && headSettled()) { recalTimer = 0; script = recalScript(); }
      } else recalTimer = 0;
      if (workload !== 'idle' && !script.length) runJobs();
    }
    servoGate += ((busy ? 1 : 0) - servoGate) * 0.05;
  }

  function runJobs() {
    if (!job) { if (!jobs.length) refillJobs(); job = jobs.shift(); jobT = 0; jobStarted = false; }
    const fr = rpm / 60;
    switch (job.k) {
      case 'seek':
        if (!jobStarted) {
          if (job.save) retX = isStepper() ? tTrack / (p.cylinders - 1) : xt;
          seekTo(job.ret ? retX : mapX(job.f));
          jobStarted = true; jobLat = (fr > 0 ? rnd() / fr : 0) + 0.0004;
        }
        busy = true;
        if (flood) { if (jobT >= 0.002) job = null; }
        else if (headSettled()) { jobLat -= ctrlDt; if (jobLat <= 0) job = null; }
        break;
      case 'seq': {
        busy = true;
        // One cylinder every (heads) revolutions: the head switches surfaces first.
        const cylTime = fr > 0 ? (2 * p.platters) / fr : 1;
        seqAcc += ctrlDt;
        if (seqAcc >= cylTime) {
          seqAcc -= cylTime;
          const step = 1 / Math.max(2, p.cylinders - 1);
          const cur = isStepper() ? tTrack / (p.cylinders - 1) : xt;
          if (cur + step <= 1) seekTo(cur + step, p.tick);
        }
        if (jobT >= job.ms / 1000) job = null;
        break;
      }
      default: // pause
        if (jobT >= job.ms / 1000) job = null;
    }
    jobT += ctrlDt;
  }

  // ---- telemetry ----
  const LAYERS = 5;
  const layerSq = new Float64Array(LAYERS);
  let telN = 0, hist = [], peak = 0;

  // ---- public API ----
  let inited = false;
  function setParams(np) {
    const wasStepper = p.actuator === 'stepper';
    p = Object.assign({}, np);
    if (!inited) { inited = true; if (p.actuator !== 'stepper' && !powered) { x = xt = parkX(); } }
    const T = Math.max(0.004, p.fullStrokeMs / 1000);
    A = 4.6 / (T * T);                               // bang-bang full stroke in about T
    zeta = clamp(p.settleDamp, 0.05, 1.5);
    w = 4 / (zeta * Math.max(0.0003, p.trackMs / 1000));
    excK = 1 - Math.exp(-TAU * (1500 + p.hardness * 14000) / sr);
    tuneModes(); tuneEq();
    if (isStepper() !== wasStepper) {             // switching actuator: carry the position over
      const cyl = Math.max(2, p.cylinders | 0);
      track = tTrack = clamp(Math.round(clamp(x, 0, 1) * (cyl - 1)), 0, cyl - 1);
      xt = x = track / (cyl - 1); v = 0; vcMode = 1;
    }
    if (isStepper()) { track = Math.min(track, p.cylinders - 1); tTrack = Math.min(tTrack, p.cylinders - 1); }
  }

  function power(on) {
    if (on === powered) return;
    powered = on;
    if (on) {
      state = 'spinup'; script = []; job = null; jobs.length = 0;
      if (isStepper()) click(0.6, 1.0);           // the stepper energises: one thunk
    } else {
      state = 'off'; script = []; job = null; jobs.length = 0;
      if (!isStepper() && p.park !== 'none') { retract = true; parkPending = true; seekTo(parkX()); }
    }
  }

  function handle(m) {
    if (m.type === 'params') setParams(m.p);
    else if (m.type === 'power') power(m.on);
    else if (m.type === 'workload') { workload = m.name; job = null; jobs.length = 0; }
    else if (m.type === 'flood') flood = !!m.on;
    else if (m.type === 'seekX') { if (state === 'ready') { script = []; job = null; jobs.length = 0; workload = 'idle'; seekTo(m.x); } }
  }

  function process(out, n) {
    for (let i = 0; i < n; i++) {
      if (++ctrlCount >= ctrlEvery) {
        ctrlCount = 0; control();
        hist.push(isStepper() ? track / (p.cylinders - 1) : x, v);
      }
      t += dt;
      const fr = rpm / 60;

      // Spindle: rotation hum, the motor's slot whine and its electrical frequency.
      phRot += fr * dt; if (phRot >= 1) phRot -= 1;
      phWhine += fr * p.slots * dt; if (phWhine >= 1) phWhine -= 1;
      phElec += fr * p.polePairs * dt; if (phElec >= 1) phElec -= 1;
      const rot = Math.sin(TAU * phRot);
      const am = 1 + p.imbalance * 0.5 * rot;
      const spin = (humAmp * (rot + 0.5 * Math.sin(2 * TAU * phRot))
        + whineAmp * (Math.sin(TAU * phWhine) + 0.35 * Math.sin(2 * TAU * phWhine))
        + elecAmp * Math.sin(TAU * phElec)) * am;

      // Bearing: band-passed noise plus a defect tone; CSS heads scraping while landing.
      phBear += fr * 4.37 * dt; if (phBear >= 1) phBear -= 1;
      runSvf(bearBp, noise());
      let bear = bearBp.bp * bearAmp * am + bearToneAmp * Math.sin(TAU * phBear);
      if (scrapeAmp > 0) { runSvf(scrapeBp, noise()); bear += scrapeBp.bp * scrapeAmp; }

      // Windage: air over the platters.
      runSvf(windLp, noise()); runSvf(windBp, noise());
      const wind = (windLp.lp * 0.7 + windBp.bp * 0.6) * windAmp * (1 + p.imbalance * 0.15 * rot);

      // Actuator
      let exc = 0;
      if (isStepper()) {
        if (stepTimer > 0) stepTimer--;
        if (stepTimer <= 0 && track !== tTrack) {
          track += track < tTrack ? 1 : -1; stepsDone++;
          const remaining = Math.abs(tTrack - track);
          const k = Math.min(stepsDone, remaining + 1);
          const ms = p.stepMs * (1 + 4 * p.stepRamp * Math.exp(-k / (3 + 12 * p.stepRamp)));
          stepTimer += (ms / 1000) * sr;  // += keeps the fraction: no drift over a long seek
          exc += 0.55 * (0.7 + 0.3 * rnd());
          if (remaining === 0) { stSettle = 0.012; caseAcc += 0.35; }
        } else if (stepTimer < 0) stepTimer = 0;
        const xs = track / (p.cylinders - 1);
        x += (xs - x) * 0.01; v = (x - xPrev) * sr; xPrev = x;
      } else {
        const d = xt - x;
        if (vcMode === 0 && Math.abs(d) < SETTLE_WIN) { vcMode = 1; click(clickAmp(seekD) * 0.45, 0.15 * clickAmp(seekD)); }
        const Ar = retract ? A * 1.5 : A;
        let a;
        if (vcMode === 0) {
          const ad = Math.abs(d);
          const vdes = (d > 0 ? 1 : -1) * Math.sqrt(2 * Ar * 0.8 * ad);
          a = clamp((vdes - v) * 4000, -Ar, Ar);
        } else {
          a = clamp(w * w * d - 2 * zeta * w * v, -Ar, Ar);
        }
        v += a * dt; x += v * dt;
        exc += (a - aPrev) / A * 0.5;
        exc += p.buzz * Math.abs(a) / A * noise() * 0.08;
        aPrev = a;
      }
      exc += clickAcc; clickAcc = 0;
      excLp += (exc - excLp) * excK;
      const caseIn = caseAcc; caseAcc = 0;
      let act = 0;
      for (let m = 0; m < NM; m++) {
        const inp = m < ARM.length ? excLp : excLp * 0.4 + caseIn;
        const y = mc1[m] * my1[m] + mc2[m] * my2[m] + mg[m] * inp;
        my2[m] = my1[m]; my1[m] = y; act += y;
      }
      act *= 0.16;

      // Servo: the track-following buzz at the servo wedge rate while transferring.
      let servo = 0;
      if (p.wedges > 0 && servoGate > 0.01) {
        phServo += fr * p.wedges * dt; if (phServo >= 1) phServo -= 1;
        runSvf(servoLp, phServo < 0.1 ? 1 : 0);
        servo = servoLp.bp * p.servo * 0.25 * servoGate;
      }

      const L0 = spin * p.gSpindle, L1 = bear * p.gBearing, L2 = wind * p.gWind, L3 = act * p.gAct, L4 = servo * p.gServo;
      layerSq[0] += L0 * L0; layerSq[1] += L1 * L1; layerSq[2] += L2 * L2; layerSq[3] += L3 * L3; layerSq[4] += L4 * L4;
      let mix = L0 + L1 + L2 + L3 + L4;
      mix = runBiquad(eqHigh, runBiquad(eqCase, runBiquad(eqLow, mix)));
      mix *= p.master;
      const y = Math.tanh(mix);
      const ay = Math.abs(y); if (ay > peak) peak = ay;
      out[i] = y;
      telN++;
    }
  }

  function telemetry() {
    const rms = [];
    for (let i = 0; i < LAYERS; i++) { rms.push(Math.sqrt(layerSq[i] / Math.max(1, telN))); layerSq[i] = 0; }
    const tel = {
      x: isStepper() ? track / (p.cylinders - 1) : x, v, rpm, state, busy, powered,
      track: isStepper() ? track : Math.round(clamp(x, 0, 1) * (p.cylinders - 1)),
      lastSeekMs, lastSeekD, seekCount, rms, peak, hist,
    };
    telN = 0; peak = 0; hist = [];
    return tel;
  }

  return { setParams, handle, process, telemetry };
}

// ---- parameter schema and presets (main thread and tests; not part of the worklet) ----
const PARAMS = [
  // group, key, label, kind, min, max, step, unit
  ['spindle', 'rpm', 'Spindle speed', 'range', 1800, 15000, 100, 'rpm'],
  ['spindle', 'spinupS', 'Spin-up time', 'range', 1, 20, 0.5, 's'],
  ['spindle', 'coastS', 'Coast-down time', 'range', 2, 30, 0.5, 's'],
  ['spindle', 'slots', 'Motor slots', 'range', 6, 18, 3, ''],
  ['spindle', 'polePairs', 'Motor pole pairs', 'range', 2, 8, 1, ''],
  ['spindle', 'whine', 'Motor whine', 'range', 0, 1, 0.01, ''],
  ['spindle', 'hum', 'Rotation hum', 'range', 0, 1, 0.01, ''],
  ['spindle', 'imbalance', 'Imbalance wobble', 'range', 0, 1, 0.01, ''],
  ['platters', 'platters', 'Platters', 'range', 1, 10, 1, ''],
  ['platters', 'size', 'Form factor', 'select', ['5.25', '3.5', '2.5'], ['5.25″', '3.5″', '2.5″']],
  ['platters', 'windage', 'Windage', 'range', 0, 1, 0.01, ''],
  ['platters', 'zoned', 'Zoned recording', 'check'],
  ['bearing', 'bearing', 'Bearing', 'select', ['ball', 'fdb'], ['Ball bearing', 'Fluid (FDB)']],
  ['bearing', 'wear', 'Wear', 'range', 0, 1, 0.01, ''],
  ['bearing', 'bearingLvl', 'Bearing noise', 'range', 0, 1, 0.01, ''],
  ['actuator', 'actuator', 'Actuator', 'select', ['voice', 'stepper'], ['Voice coil', 'Stepper motor']],
  ['actuator', 'cylinders', 'Cylinders', 'range', 300, 100000, 1, ''],
  ['actuator', 'fullStrokeMs', 'Full-stroke seek', 'range', 8, 60, 1, 'ms', 'voice'],
  ['actuator', 'trackMs', 'Track-to-track', 'range', 0.5, 8, 0.1, 'ms', 'voice'],
  ['actuator', 'settleDamp', 'Settle damping', 'range', 0.1, 1.2, 0.01, '', 'voice'],
  ['actuator', 'buzz', 'Coil buzz', 'range', 0, 1, 0.01, '', 'voice'],
  ['actuator', 'stepMs', 'Step interval', 'range', 0.2, 6, 0.1, 'ms', 'stepper'],
  ['actuator', 'stepRamp', 'Buffered-seek ramp', 'range', 0, 1, 0.01, '', 'stepper'],
  ['actuator', 'tick', 'Track tick', 'range', 0, 1, 0.01, ''],
  ['resonance', 'armHz', 'Arm resonance', 'range', 200, 4000, 10, 'Hz'],
  ['resonance', 'armRingMs', 'Arm ring', 'range', 5, 200, 1, 'ms'],
  ['resonance', 'caseHz', 'Case resonance', 'range', 80, 1200, 5, 'Hz'],
  ['resonance', 'caseRingMs', 'Case ring', 'range', 10, 400, 1, 'ms'],
  ['resonance', 'hardness', 'Click hardness', 'range', 0, 1, 0.01, ''],
  ['heads', 'park', 'Head parking', 'select', ['ramp', 'zone', 'none'], ['Load/unload ramp', 'Landing zone (CSS)', 'None']],
  ['heads', 'calSeeks', 'Calibration seeks', 'range', 0, 8, 1, ''],
  ['heads', 'recalS', 'Thermal recal every', 'range', 0, 120, 1, 's'],
  ['heads', 'wedges', 'Servo wedges', 'range', 0, 400, 1, ''],
  ['heads', 'servo', 'Servo buzz', 'range', 0, 1, 0.01, ''],
  ['enclosure', 'mount', 'Mounting', 'select', ['bare', 'case', 'sled'], ['Bare on the desk', 'In a PC case', 'Rubber sled']],
  ['enclosure', 'bright', 'Brightness', 'range', -1, 1, 0.01, ''],
  ['enclosure', 'caseRes', 'Case boom', 'range', 0, 1, 0.01, ''],
];

const MIX_DEFAULTS = { gSpindle: 1, gBearing: 1, gWind: 1, gAct: 1, gServo: 1, master: 0.8 };

const PRESETS = [
  {
    id: 'stepper85', name: '1985 stepper', note: '5.25″ MFM, 3600 rpm, 615 cylinders',
    p: { rpm: 3600, spinupS: 12, coastS: 16, slots: 12, polePairs: 4, whine: 0.6, hum: 0.5, imbalance: 0.3,
      platters: 2, size: '5.25', windage: 0.7, zoned: false, bearing: 'ball', wear: 0.55, bearingLvl: 0.75,
      actuator: 'stepper', cylinders: 615, fullStrokeMs: 40, trackMs: 3, settleDamp: 0.5, buzz: 0, stepMs: 0.8, stepRamp: 0.5, tick: 0.6,
      armHz: 900, armRingMs: 35, caseHz: 210, caseRingMs: 140, hardness: 0.6,
      park: 'none', calSeeks: 2, recalS: 0, wedges: 0, servo: 0, mount: 'case', bright: 0, caseRes: 0.6 },
  },
  {
    id: 'voice91', name: '1991 voice coil', note: '3.5″, 3600 rpm, landing zone',
    p: { rpm: 3600, spinupS: 6, coastS: 10, slots: 9, polePairs: 3, whine: 0.5, hum: 0.4, imbalance: 0.2,
      platters: 2, size: '3.5', windage: 0.5, zoned: false, bearing: 'ball', wear: 0.35, bearingLvl: 0.6,
      actuator: 'voice', cylinders: 1000, fullStrokeMs: 35, trackMs: 5, settleDamp: 0.35, buzz: 0.4, stepMs: 3, stepRamp: 0, tick: 0.4,
      armHz: 650, armRingMs: 60, caseHz: 300, caseRingMs: 90, hardness: 0.5,
      park: 'zone', calSeeks: 4, recalS: 0, wedges: 0, servo: 0, mount: 'case', bright: -0.2, caseRes: 0.5 },
  },
  {
    id: 'voice97', name: '1997 voice coil', note: '3.5″, 5400 rpm, thermal recal',
    p: { rpm: 5400, spinupS: 5, coastS: 9, slots: 12, polePairs: 4, whine: 0.45, hum: 0.35, imbalance: 0.15,
      platters: 3, size: '3.5', windage: 0.6, zoned: true, bearing: 'ball', wear: 0.3, bearingLvl: 0.5,
      actuator: 'voice', cylinders: 6000, fullStrokeMs: 19, trackMs: 2, settleDamp: 0.45, buzz: 0.3, stepMs: 3, stepRamp: 0, tick: 0.35,
      armHz: 1100, armRingMs: 35, caseHz: 380, caseRingMs: 70, hardness: 0.75,
      park: 'zone', calSeeks: 5, recalS: 45, wedges: 72, servo: 0.3, mount: 'case', bright: 0, caseRes: 0.4 },
  },
  {
    id: 'fdb05', name: '2005 7200 rpm', note: '3.5″, fluid bearing, ramp load',
    p: { rpm: 7200, spinupS: 6, coastS: 12, slots: 9, polePairs: 4, whine: 0.25, hum: 0.2, imbalance: 0.05,
      platters: 2, size: '3.5', windage: 0.5, zoned: true, bearing: 'fdb', wear: 0.1, bearingLvl: 0.4,
      actuator: 'voice', cylinders: 80000, fullStrokeMs: 14, trackMs: 0.8, settleDamp: 0.6, buzz: 0.15, stepMs: 3, stepRamp: 0, tick: 0.25,
      armHz: 1800, armRingMs: 20, caseHz: 450, caseRingMs: 50, hardness: 0.9,
      park: 'ramp', calSeeks: 3, recalS: 0, wedges: 200, servo: 0.2, mount: 'case', bright: 0.2, caseRes: 0.3 },
  },
];

if (typeof module !== 'undefined') module.exports = { makeEngine, PARAMS, PRESETS, MIX_DEFAULTS };
