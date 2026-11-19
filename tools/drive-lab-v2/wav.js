// Minimal WAV I/O, for the Node tools and the Lab page (which has no fs):
// parseWav() takes the file's bytes, encodeWav() gives 16-bit mono bytes.
// In: PCM 8/16/24/32-bit or 32-bit float, mono or more channels (averaged), any
// extra chunks skipped. The browser's own decoder isn't used, as it resamples.

function parseWav(bytes, what) {
  const u8 = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  const v = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  const tag = (p) => String.fromCharCode(u8[p], u8[p + 1], u8[p + 2], u8[p + 3]);
  what = what || 'file';
  if (u8.length < 12 || tag(0) !== 'RIFF' || tag(8) !== 'WAVE') throw new Error(what + ': not a WAV');
  let p = 12, sr = 0, ch = 1, bits = 16, fmt = 1, data = null;
  while (p + 8 <= u8.length) {
    const id = tag(p), size = v.getUint32(p + 4, true), body = p + 8;
    if (id === 'fmt ') {
      fmt = v.getUint16(body, true); ch = v.getUint16(body + 2, true); sr = v.getUint32(body + 4, true); bits = v.getUint16(body + 14, true);
      if (fmt === 0xFFFE && size >= 26) fmt = v.getUint16(body + 24, true);   // WAVE_FORMAT_EXTENSIBLE: the real format
    } else if (id === 'data') data = [body, Math.min(u8.length, body + size)];
    p = body + size + (size & 1);
  }
  if (!data || !sr) throw new Error(what + ': no audio in it');
  const float = fmt === 3;
  if (!(fmt === 1 || float) || (float && bits !== 32) || ![8, 16, 24, 32].includes(bits)) throw new Error(`${what}: unsupported WAV (format ${fmt}, ${bits}-bit)`);
  const bps = bits / 8, frames = Math.floor((data[1] - data[0]) / (bps * ch)), x = new Float64Array(frames);
  const read = float ? (o) => v.getFloat32(o, true)
    : bits === 8 ? (o) => (u8[o] - 128) / 128
    : bits === 16 ? (o) => v.getInt16(o, true) / 32768
    : bits === 24 ? (o) => ((u8[o] | (u8[o + 1] << 8) | (u8[o + 2] << 16)) << 8 >> 8) / 8388608
    : (o) => v.getInt32(o, true) / 2147483648;
  for (let i = 0; i < frames; i++) {
    let s = 0;
    for (let c = 0; c < ch; c++) s += read(data[0] + (i * ch + c) * bps);
    x[i] = s / ch;
  }
  return { sr, x };
}

function encodeWav(x, sr) {
  const n = x.length, u8 = new Uint8Array(44 + n * 2), v = new DataView(u8.buffer);
  const put = (p, s) => { for (let i = 0; i < s.length; i++) u8[p + i] = s.charCodeAt(i); };
  put(0, 'RIFF'); v.setUint32(4, 36 + n * 2, true); put(8, 'WAVEfmt '); v.setUint32(16, 16, true);
  v.setUint16(20, 1, true); v.setUint16(22, 1, true); v.setUint32(24, sr, true); v.setUint32(28, sr * 2, true);
  v.setUint16(32, 2, true); v.setUint16(34, 16, true); put(36, 'data'); v.setUint32(40, n * 2, true);
  for (let i = 0; i < n; i++) v.setInt16(44 + i * 2, Math.max(-32767, Math.min(32767, Math.round(x[i] * 32767))), true);
  return u8;
}

function readWav(file) { return parseWav(require('fs').readFileSync(file), file); }
function writeWav(file, x, sr) { require('fs').writeFileSync(file, encodeWav(x, sr)); }

module.exports = { parseWav, encodeWav, readWav, writeWav };
