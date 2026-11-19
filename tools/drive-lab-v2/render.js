// Renders a drive as a recorded pack would be: spinup.wav, idle.wav, spindown.wav.
//
//   node render.js drive.json OUT_DIR [sample_rate]
const path = require('path'), fs = require('fs');
const { makeEngine } = require('./engine.js');

const BLK = 128;

function run(e, secs, sr) {
  const n = Math.round(secs * sr / BLK), out = new Float64Array(n * BLK), buf = new Float32Array(BLK);
  for (let b = 0; b < n; b++) { e.process(buf, BLK); out.set(buf, b * BLK); }
  return out;
}

// Idle only, starting at full speed.
function renderIdle(drive, sr, secs) {
  const e = makeEngine(sr);
  e.setParams(drive); e.handle({ type: 'ready' });
  run(e, 0.2, sr);                                   // let the level ramps settle
  return run(e, secs, sr);
}

// Busy (seeking) at full speed. seeksOnly drops the spindle's tones and noise, so the
// fitter can add the seeks to a recorded idle instead.
function renderAccess(drive, sr, secs, opts) {
  opts = opts || {};
  const dr = opts.seeksOnly ? { ...drive, tones: [], noise: { db: [] } } : drive;
  const e = makeEngine(sr);
  e.setParams(dr); e.handle({ type: 'ready' });
  run(e, 0.2, sr);
  e.handle({ type: 'busy', on: true, cls: opts.cls || 'random' });
  return run(e, secs, sr);
}

function renderPack(drive, sr) {
  const e = makeEngine(sr);
  e.setParams(drive);
  e.handle({ type: 'power', on: true });
  const spinup = run(e, drive.spindle.spinupS + 1.5, sr);
  const idle = run(e, 4, sr);
  e.handle({ type: 'busy', on: true });
  const access = run(e, 6, sr);
  e.handle({ type: 'busy', on: false });
  run(e, 0.1, sr);
  e.handle({ type: 'power', on: false });
  const spindown = run(e, drive.spindle.coastS + 0.5, sr);
  return { spinup, idle, access, spindown };
}

if (require.main === module) {
  const [file, out, rate] = process.argv.slice(2);
  if (!file || !out) { console.error('usage: node render.js drive.json OUT_DIR [sample_rate]'); process.exit(2); }
  const { writeWav } = require('./wav.js');
  const sr = Number(rate) || 16000, drive = JSON.parse(fs.readFileSync(file, 'utf8'));
  fs.mkdirSync(out, { recursive: true });
  const p = renderPack(drive, sr);
  for (const k of Object.keys(p)) writeWav(path.join(out, k + '.wav'), p[k], sr);
  console.log('rendered', file, '->', out, '@', sr, 'Hz');
}

module.exports = { renderIdle, renderAccess, renderPack };
