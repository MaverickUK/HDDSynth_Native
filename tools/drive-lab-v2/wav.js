// Minimal WAV I/O for the Node tools: 16-bit PCM in (mono or stereo, any extra
// chunks skipped), 16-bit mono out.
const fs = require('fs');

function readWav(file) {
  const b = fs.readFileSync(file);
  if (b.toString('ascii', 0, 4) !== 'RIFF' || b.toString('ascii', 8, 12) !== 'WAVE') throw new Error(file + ': not a WAV');
  let p = 12, sr = 0, ch = 1, bits = 16, data = null;
  while (p + 8 <= b.length) {
    const id = b.toString('ascii', p, p + 4), size = b.readUInt32LE(p + 4), body = p + 8;
    if (id === 'fmt ') { ch = b.readUInt16LE(body + 2); sr = b.readUInt32LE(body + 4); bits = b.readUInt16LE(body + 14); }
    else if (id === 'data') data = b.subarray(body, Math.min(b.length, body + size));
    p = body + size + (size & 1);
  }
  if (!data || bits !== 16) throw new Error(file + ': need 16-bit PCM');
  const frames = Math.floor(data.length / (2 * ch)), x = new Float64Array(frames);
  for (let i = 0; i < frames; i++) {
    let s = 0;
    for (let c = 0; c < ch; c++) s += data.readInt16LE((i * ch + c) * 2);
    x[i] = s / ch / 32768;
  }
  return { sr, x };
}

function writeWav(file, x, sr) {
  const n = x.length, b = Buffer.alloc(44 + n * 2);
  b.write('RIFF', 0); b.writeUInt32LE(36 + n * 2, 4); b.write('WAVEfmt ', 8); b.writeUInt32LE(16, 16);
  b.writeUInt16LE(1, 20); b.writeUInt16LE(1, 22); b.writeUInt32LE(sr, 24); b.writeUInt32LE(sr * 2, 28);
  b.writeUInt16LE(2, 32); b.writeUInt16LE(16, 34); b.write('data', 36); b.writeUInt32LE(n * 2, 40);
  for (let i = 0; i < n; i++) b.writeInt16LE(Math.max(-32767, Math.min(32767, Math.round(x[i] * 32767))), 44 + i * 2);
  fs.writeFileSync(file, b);
}

module.exports = { readWav, writeWav };
