"""(I24) SOUND FM: the music pack's samples (little-endian pack, mkmusic.py ... le) and their features."""
import math, struct
import numpy as np

class Pack:
    def __init__(self, path):
        d = open(path, 'rb').read()
        assert d[:4] == b'MUS1'
        self.d = d
        self.nsongs, self.nsamples = struct.unpack_from('<HH', d, 4)
        self.off_songs, self.off_samples = struct.unpack_from('<II', d, 8)
    def sample(self, i):
        off, n, loop = struct.unpack_from('<III', self.d, self.off_samples + 12 * i)
        pcm = np.frombuffer(self.d, dtype='<i2', count=n + 1, offset=off).astype(np.float64) / 16384.0
        return pcm[:n], loop

def f0_of(x, sr=32000.0):
    """fundamental of a (looping part of a) sample played at 32 kHz: autocorrelation peak"""
    x = x - x.mean()
    if len(x) < 64 or np.abs(x).max() < 1e-4: return 0.0
    n = len(x)
    f = np.fft.rfft(x, 2 * n)
    ac = np.fft.irfft(f * np.conj(f))[:n]
    ac /= ac[0] + 1e-12
    lo = int(sr / 4000); hi = min(n - 1, int(sr / 25))
    if hi <= lo + 2: return 0.0
    k = lo + int(np.argmax(ac[lo:hi]))
    if ac[k] < 0.3: return 0.0
    # (parabolic peak; prefer the first strong peak - avoid octave errors)
    for j in range(lo, k):
        if ac[j] > 0.85 * ac[k] and ac[j] >= ac[j - 1] and ac[j] >= ac[j + 1]: k = j; break
    a, b, c = ac[k - 1], ac[k], ac[k + 1]
    p = 0.5 * (a - c) / (a - 2 * b + c) if (a - 2 * b + c) else 0.0
    p = max(-0.5, min(0.5, p))
    return sr / (k + p)

def features(pcm, loop):
    n = len(pcm)
    looped = loop < n
    body = pcm[loop:] if looped and n - loop >= 32 else pcm[:min(n, 4096)]
    if looped and len(body) < 2048:            # (a short loop: repeat it for the analysis)
        body = np.tile(body, 2048 // max(len(body), 1) + 1)[:2048]
    f0 = f0_of(body)
    seg = pcm[:min(n, 4096)]
    sp = np.abs(np.fft.rfft(seg * np.hanning(len(seg)))) + 1e-9
    flat = math.exp(np.mean(np.log(sp))) / np.mean(sp)          # spectral flatness: ~1 noise, ~0 tone
    cent = float((sp * np.arange(len(sp))).sum() / sp.sum()) * 32000.0 / len(seg)
    env = [float(np.sqrt(np.mean(pcm[i:i + 256] ** 2))) for i in range(0, min(n, 32 * 256), 256)]
    return dict(n=n, looped=looped, loop=loop, f0=f0, flat=flat, centroid=cent, env=env, peak=float(np.abs(pcm).max()))

def f0_line(pcm, loop, sr=32000.0, keep=0.97):
    """a loop of L samples repeats exactly: its spectrum is lines at m*sr/L.  The fundamental is the largest
    k*sr/L such that the multiples of k hold `keep` of the energy (k = cycles in the loop)."""
    n = len(pcm); L = n - loop
    if loop >= n or L < 8: return 0.0
    x = pcm[loop:] - pcm[loop:].mean()
    X = np.abs(np.fft.rfft(x)) ** 2                     # one loop period: bin m = m*sr/L exactly
    X[0] = 0
    tot = X.sum()
    if tot <= 0: return 0.0
    best = 1
    for k in range(1, L // 2 + 1):
        if sr * k / L > 4000: break
        if X[k::k].sum() >= keep * tot: best = k
    return sr * best / L

def harmonic_capture(spec, freqs, f0, rel=0.06):
    """fraction of the spectrum's energy within +-rel*f0 of the multiples of f0"""
    if f0 <= 0: return 0.0
    k = np.round(freqs / f0)
    near = (k >= 1) & (np.abs(freqs - k * f0) <= rel * f0)
    tot = spec[freqs > 20].sum()
    return float(spec[near].sum() / tot) if tot > 0 else 0.0

def f0_pick(pcm, loop, f_ac, sr=32000.0, need=0.78):
    """the fundamental: the highest candidate (autocorrelation, its divisions, the loop's own periods) whose
    harmonics hold `need` of the energy"""
    n = len(pcm)
    looped = loop < n and n - loop >= 8
    if looped:
        L = n - loop
        body = np.tile(pcm[loop:], 131072 // L + 1)[:131072]
    else:
        body = pcm[:min(n, 8192)]
        if len(body) < 256: return f_ac if f_ac > 0 else 0.0
    body = body - body.mean()
    S = np.abs(np.fft.rfft(body * np.hanning(len(body)))) ** 2
    fr = np.fft.rfftfreq(len(body), 1 / sr)
    cands = set()
    if f_ac > 0:
        for d in (1, 2, 3, 4, 5, 6): cands.add(f_ac / d)
    if looped and L <= 1024:                 # (short loops: their own periods; long loops make dense candidates)
        for k in range(1, L // 2 + 1):
            f = sr * k / L
            if f > 4000: break
            if f >= 20: cands.add(f)
    best, bestc = 0.0, 0.0
    for f in sorted(cands):
        if f < 20: continue
        c = harmonic_capture(S, fr, f)
        if c >= need: best, bestc = f, c
    if best == 0.0 and f_ac > 0:
        best = f_ac
    # (refine: the harmonics' sharpest fit within +-1.5 %, a narrow window)
    if best > 0:
        grid = best * (1 + np.linspace(-0.015, 0.015, 121))
        sc = [harmonic_capture(S, fr, f, rel=0.012) for f in grid]
        best = float(grid[int(np.argmax(sc))])
    # (snap to the loop: a loop holds a whole number of periods)
    if looped and best > 0 and L <= 8192:
        k = max(1, round(L * best / sr)); best = sr * k / L
    return best
