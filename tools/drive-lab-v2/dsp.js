// Spectral analysis helpers shared by the fitter (fit.js) and the scorer (score.js).
// Plain JS with no dependencies, so the Lab page can use them in the browser too.

// In-place radix-2 complex FFT. re.length must be a power of two.
function fft(re, im) {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { let t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const ang = -2 * Math.PI / len, wr = Math.cos(ang), wi = Math.sin(ang);
    for (let i = 0; i < n; i += len) {
      let cr = 1, ci = 0;
      for (let k = 0; k < len / 2; k++) {
        const a = i + k, b = a + len / 2;
        const xr = re[b] * cr - im[b] * ci, xi = re[b] * ci + im[b] * cr;
        re[b] = re[a] - xr; im[b] = im[a] - xi; re[a] += xr; im[a] += xi;
        const t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
      }
    }
  }
}

function makeWindow(n, type) {
  const w = new Float64Array(n);
  for (let i = 0; i < n; i++) {
    const x = 2 * Math.PI * i / n;
    w[i] = type === 'bh'   // Blackman-Harris: low leakage, so weak tones show beside strong ones
      ? 0.35875 - 0.48829 * Math.cos(x) + 0.14128 * Math.cos(2 * x) - 0.01168 * Math.cos(3 * x)
      : 0.5 - 0.5 * Math.cos(x);
  }
  return w;
}

const pow2Below = (len, cap) => { let n = 1; while (n * 2 <= Math.min(len, cap)) n *= 2; return n; };

// One-sided power spectrum of x[from..from+n), scaled as a PSD (power per Hz).
function psdFrame(x, from, n, w, sr, wsq) {
  const re = new Float64Array(n), im = new Float64Array(n);
  for (let i = 0; i < n; i++) re[i] = (x[from + i] || 0) * w[i];
  fft(re, im);
  const P = new Float64Array(n / 2 + 1), sc = 1 / (sr * wsq);
  for (let k = 0; k <= n / 2; k++) P[k] = (re[k] * re[k] + im[k] * im[k]) * sc * (k === 0 || k === n / 2 ? 1 : 2);
  return P;
}

// Welch PSD: the average of half-overlapped windowed frames.
// Integrating P over the bins of a sinusoid gives its power, A^2/2.
function welch(x, sr, n, type) {
  n = pow2Below(x.length, n);
  const w = makeWindow(n, type || 'hann');
  let wsq = 0; for (let i = 0; i < n; i++) wsq += w[i] * w[i];
  const P = new Float64Array(n / 2 + 1);
  let frames = 0;
  for (let s = 0; s + n <= x.length; s += n / 2) {
    const F = psdFrame(x, s, n, w, sr, wsq);
    for (let k = 0; k < P.length; k++) P[k] += F[k];
    frames++;
  }
  for (let k = 0; k < P.length; k++) P[k] /= frames;
  const df = sr / n, f = new Float64Array(P.length);
  for (let k = 0; k < f.length; k++) f[k] = k * df;
  return { f, P, df, n };
}

// Short-time PSD frames: one spectrum every hop samples.
function frames(x, sr, n, hop, type) {
  const w = makeWindow(n, type || 'hann');
  let wsq = 0; for (let i = 0; i < n; i++) wsq += w[i] * w[i];
  const out = [], t = [];
  for (let s = 0; s + n <= x.length; s += hop) { out.push(psdFrame(x, s, n, w, sr, wsq)); t.push((s + n / 2) / sr); }
  return { S: out, t, df: sr / n };
}

function medfilt(a, w) {
  const h = w >> 1, out = new Float64Array(a.length), buf = [];
  for (let i = 0; i < a.length; i++) {
    buf.length = 0;
    for (let j = Math.max(0, i - h); j <= Math.min(a.length - 1, i + h); j++) buf.push(a[j]);
    buf.sort((p, q) => p - q);
    out[i] = buf[buf.length >> 1];
  }
  return out;
}

// Local maxima at least minH high, strongest first, no two closer than minDist.
function findPeaks(a, minH, minDist) {
  const cand = [];
  for (let i = 1; i < a.length - 1; i++) if (a[i] >= minH && a[i] >= a[i - 1] && a[i] > a[i + 1]) cand.push(i);
  cand.sort((p, q) => a[q] - a[p]);
  const keep = [];
  for (const i of cand) if (keep.every((k) => Math.abs(k - i) >= minDist)) keep.push(i);
  return keep;
}

// Power between lo and hi Hz (integral of the PSD).
function bandPower(f, P, df, lo, hi) {
  let s = 0;
  for (let k = 0; k < P.length; k++) if (f[k] >= lo && f[k] < hi) s += P[k];
  return s * df;
}

const thirdOctaves = (sr) => {
  const c = [];
  for (let i = 0; ; i++) { const f = 31.5 * Math.pow(2, i / 3); if (f * Math.pow(2, 1 / 6) >= sr / 2) break; c.push(f); }
  return c;
};

// Level in dB of each third-octave band.
function thirdOctaveDb(x, sr) {
  const { f, P, df } = welch(x, sr, 4096);
  return thirdOctaves(sr).map((c) => 10 * Math.log10(bandPower(f, P, df, c * Math.pow(2, -1 / 6), c * Math.pow(2, 1 / 6)) + 1e-15));
}

const rmsDb = (x) => { let s = 0; for (const v of x) s += v * v; return 10 * Math.log10(s / x.length + 1e-15); };

if (typeof module !== 'undefined') module.exports = { fft, makeWindow, welch, frames, medfilt, findPeaks, bandPower, thirdOctaves, thirdOctaveDb, rmsDb };
