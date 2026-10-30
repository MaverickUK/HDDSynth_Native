// Runs the same scenario on the prototype's engine (engine.js) and on the
// firmware's (src/drive_engine.*, through test/tools/render_drive), and checks
// that the two agree: seek times within 10%, levels and seek rates within 35%
// (their random numbers differ, so the workloads aren't sample-identical).
//
//   node tools/drive-lab/compare.js path/to/render_drive
//
// Run by test/run_native_tests.sh when node is installed.
const { execFileSync } = require('child_process');
const { makeEngine, PRESETS, MIX_DEFAULTS } = require('./engine.js');

const SR = 48000, BLK = 128;

function scenario(pr) {
  const e = makeEngine(SR), buf = new Float32Array(BLK);
  e.setParams(Object.assign({}, pr.p, MIX_DEFAULTS));
  let tel = e.telemetry();
  const run = (secs, a) => {
    const n = Math.round(secs * SR / BLK);
    for (let b = 0; b < n; b++) {
      e.process(buf, BLK);
      for (const y of buf) { a.sq += y * y; a.n++; a.pk = Math.max(a.pk, Math.abs(y)); }
      tel = e.telemetry();
    }
  };
  const acc = () => ({ sq: 0, n: 0, pk: 0 });
  const rms = (a) => Math.sqrt(a.sq / a.n);
  const r = {};
  let a = acc();
  e.handle({ type: 'power', on: true });
  let secs = 0;
  while (tel.state !== 'ready' && secs < 30) { run(0.1, a); secs += 0.1; }
  r.ready = secs; r.spinup = rms(a);
  a = acc(); run(1, a); r.idle = rms(a);
  e.handle({ type: 'seekX', x: 0 }); run(0.6, a);
  e.handle({ type: 'seekX', x: 1 }); a = acc(); run(2.5, a); r.stroke = tel.lastSeekMs;
  e.handle({ type: 'seekX', x: 0.66 }); run(1.5, a); r.third = tel.lastSeekMs;
  for (const w of ['random', 'seq', 'boot', 'defrag']) {
    e.handle({ type: 'workload', name: w });
    const s0 = tel.seekCount; a = acc(); run(4, a);
    r[w] = rms(a); r[w + 'Rate'] = (tel.seekCount - s0) / 4;
  }
  e.handle({ type: 'workload', name: 'random' }); e.handle({ type: 'flood', on: true });
  a = acc(); run(2, a); r.flood = rms(a);
  e.handle({ type: 'flood', on: false }); e.handle({ type: 'workload', name: 'idle' });
  e.handle({ type: 'power', on: false }); a = acc(); run(pr.p.coastS + 2, a); r.off = rms(a);
  return r;
}

function parseNative(text) {
  const r = {};
  const num = (re) => { const m = text.match(re); return m ? parseFloat(m[1]) : NaN; };
  r.ready = num(/ready after ([\d.]+) s/);
  r.spinup = num(/^spinup\s+rms ([\d.]+)/m);
  r.idle = num(/^idle\s+rms ([\d.]+)/m);
  r.stroke = num(/full stroke ([\d.]+) ms/);
  r.third = num(/^third\s+([\d.]+) ms/m);
  for (const w of ['random', 'seq', 'boot', 'defrag']) {
    r[w] = num(new RegExp(`^${w}\\s+rms ([\\d.]+)`, 'm'));
    r[w + 'Rate'] = num(new RegExp(`^${w}\\s+.*seeks/s ([\\d.]+)`, 'm'));
  }
  r.flood = num(/^flood\s+rms ([\d.]+)/m);
  r.off = num(/^off\s+rms ([\d.]+)/m);
  return r;
}

const bin = process.argv[2];
if (!bin) { console.error('usage: node compare.js path/to/render_drive'); process.exit(2); }
let bad = 0;
for (const pr of PRESETS) {
  const js = scenario(pr), cpp = parseNative(execFileSync(bin, [pr.id]).toString());
  for (const [k, vJs] of Object.entries(js)) {
    const vC = cpp[k];
    const tol = ['stroke', 'third', 'ready'].includes(k) ? 0.10 : 0.35;
    const floor = k.endsWith('Rate') ? 1 : k === 'ready' ? 0.2 : 0.004;  // absolute slack for small values
    const ok = Number.isFinite(vC) && Math.abs(vC - vJs) <= Math.max(floor, tol * Math.abs(vJs));
    if (!ok) { bad++; console.log(`  FAIL ${pr.id} ${k}: prototype ${vJs.toFixed(4)}, firmware ${vC}`); }
  }
}
console.log(bad ? `drive engine: ${bad} differences from the prototype` : 'drive engine matches the prototype');
process.exit(bad ? 1 : 0);
