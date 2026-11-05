"""(I24) SOUND FM: fit a YM2612 4-operator patch to a sample's sound (my own FM model, numpy).

The model (checked against a reference core: modulation 8*pi rad at TL 0, feedback (o1+o2)*pi*2^(fb-6) rad,
TL 0.75 dB a step) gives the steady spectrum of a patch; a search over algorithm, feedback, multipliers and
levels finds the patch whose harmonics are closest (in dB) to the sample's.  Only numbers come out."""
import math
import numpy as np

SR_CHIP = 7670453.0 / 144.0
MULS = np.array([0.5, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15])
MUL_W = np.array([3, 10, 7, 5, 4, 2, 2, 1, 2, 1, 1, 1, 1, 1, 1, 1], float); MUL_W /= MUL_W.sum()
# (modulators of each operator, carriers) for algorithms 0-7; operators 0-3 = S1-S4
ALG = {0: ({1: [0], 2: [1], 3: [2]}, [3]), 1: ({2: [0, 1], 3: [2]}, [3]), 2: ({2: [1], 3: [0, 2]}, [3]),
       3: ({1: [0], 3: [1, 2]}, [3]), 4: ({1: [0], 3: [2]}, [1, 3]), 5: ({1: [0], 2: [0], 3: [0]}, [1, 2, 3]),
       6: ({1: [0]}, [1, 2, 3]), 7: ({}, [0, 1, 2, 3])}
NH = 32                     # spectrum points: multiples of f0/2 up to 16 harmonics
SIN_N = 1 << 16             # (the feedback operator's sine table - see simulate)
SIN_Q = np.round(np.sin(2 * np.pi * np.arange(SIN_N) / SIN_N) * 2.0 ** 30) / 2.0 ** 30

def target_spectrum(pcm, loop, f0, sr=32000.0):
    """the sample's harmonic levels (dB, max 0) at k*f0/2, k = 1..NH"""
    n = len(pcm)
    if loop < n and n - loop >= 16:
        body = np.tile(pcm[loop:], 65536 // (n - loop) + 1)[:65536]
    else:
        body = pcm[:min(n, int(0.06 * sr))]
        body = np.concatenate([body, np.zeros(max(0, 8192 - len(body)))])
    w = np.hanning(len(body))
    P = np.abs(np.fft.rfft(body * w)) ** 2
    df = sr / len(body)
    out = np.zeros(NH)
    for k in range(1, NH + 1):                  # (the energy of a band around each point: ensembles / chorus
        f = k * f0 / 2                          #  spread a harmonic over several bins)
        if f >= sr / 2: out[k - 1] = 1e-12; continue
        i0 = max(1, int(round((f - f0 / 4) / df))); i1 = max(i0 + 1, int(round((f + f0 / 4) / df)))
        out[k - 1] = np.sqrt(P[i0:i1].sum())
    db = 20 * np.log10(out / out.max() + 1e-9)
    i_hi = int(round((NH / 2 + 0.5) * f0 / df))
    hi = 10 * np.log10(P[i_hi:].sum() / (P[1:].sum() + 1e-30) + 1e-9)
    return np.maximum(db, -60.0), hi

def simulate(alg, fb, mul, tl, f, periods=6, rms=False, hif=40):
    """C candidates: mul, tl (C, 4).  -> spectrum dB (C, NH) at k*f/2"""
    C = mul.shape[0]
    P = max(8, int(round(SR_CHIP / f)))
    T = P * periods
    t = np.arange(T)
    amp = 10 ** (-0.75 * tl / 20.0)                                   # (C, 4)
    ph = (t[None, None, :] / P) * mul[:, :, None]                      # (C, 4, T) cycles
    outs = [None] * 4
    # S1 with feedback (the only recursion)
    # (I24) bit-exact on every machine: the feedback loop is chaotic at high feedback, so a last-bit
    # difference of a maths library's sin() grew into another waveform (another patch on Windows).  Like
    # the chip: a sine table indexed by an integer phase (16 bits), levels on a 2^-30 grid - only correctly
    # rounded + - * from there on
    o1 = np.zeros(C); o2 = np.zeros(C); s1 = np.zeros((C, T))
    a0 = np.round(amp[:, 0] * 2.0 ** 30) / 2.0 ** 30
    m2 = np.round(mul[:, 0] * 2).astype(np.int64)                      # (MUL x 2: 0.5 -> 1)
    base = (t[None, :].astype(np.int64) * m2[:, None] * SIN_N) // (2 * P)   # (C, T) phase in table steps
    kq = 2.0 ** (fb - 7) * SIN_N if fb else 0.0                         # (pi 2^(fb-6) rad = 2^(fb-7) cycles)
    for i in range(T):
        idx = (base[:, i] + np.floor((o1 + o2) * kq).astype(np.int64)) & (SIN_N - 1)
        v = SIN_Q[idx] * a0
        o2 = o1; o1 = v; s1[:, i] = v
    outs[0] = s1
    mods, car = ALG[alg]
    for op in (1, 2, 3):
        m = sum(outs[j] for j in mods.get(op, [])) if op in mods else 0.0
        outs[op] = np.sin(2 * math.pi * ph[:, op, :] + 8 * math.pi * m) * amp[:, op, None]
    y = sum(outs[c] for c in car)
    seg = y[:, -2 * P * 2:]                                            # 4 periods (half-harmonics resolvable)
    Y = np.abs(np.fft.rfft(seg, axis=1))
    idx = np.arange(1, NH + 1) * 2                                     # bin of k*f/2 over 4 periods = 2k
    idx = np.minimum(idx, Y.shape[1] - 1)
    H = Y[:, idx]
    db = 20 * np.log10(H / (H.max(axis=1, keepdims=True) + 1e-12) + 1e-9)
    # energy above harmonic NH/2 up to `hif` x f (what the sample would hold there; dB re total)
    E = Y ** 2
    top = min(E.shape[1], int(2 * hif) + 1)
    hi = 10 * np.log10(E[:, 2 * (NH // 2) + 1:top].sum(axis=1) / (E[:, 1:].sum(axis=1) + 1e-30) + 1e-9)
    if rms: return np.maximum(db, -60.0), np.sqrt((seg ** 2).mean(axis=1))
    return np.maximum(db, -60.0), hi

def distance(sim, target, fplay):
    db, hi = sim
    tg, thi = target
    k = np.arange(1, NH + 1)
    w = 1.0 / (1.0 + 0.15 * k) * ((k * fplay / 2) < 9000)
    w = w * np.where(tg > -45, 1.0, 0.4)
    e = ((db - tg[None, :]) ** 2 * w[None, :]).sum(axis=1) / w.sum()
    return e + 0.5 * np.maximum(0.0, hi - max(thi, -40.0)) ** 2 / 4     # (soft: FM is brighter by nature)

class Rng:
    """(I24) the fit's random numbers: splitmix64 in plain integers, uniform -> exact arithmetic only (no libm,
    no numpy Generator - its streams may change between numpy versions): the same patches on every machine
    (the Windows builder's numpy is not this one's).  Normal = Irwin-Hall (12 uniforms)."""
    M = (1 << 64) - 1

    def __init__(self, seed):
        self.s = seed & self.M

    def _u64(self, n):
        out = []
        s = self.s
        for _ in range(n):
            s = (s + 0x9E3779B97F4A7C15) & self.M
            z = s
            z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & self.M
            z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & self.M
            out.append(z ^ (z >> 31))
        self.s = s
        return out

    def random(self, size):
        n = int(np.prod(size))
        return (np.array([v >> 11 for v in self._u64(n)], dtype=np.float64) * (1.0 / (1 << 53))).reshape(size)

    def uniform(self, lo, hi, size):
        return lo + (hi - lo) * self.random(size)

    def normal(self, mu, sigma, size):
        n = int(np.prod(size)) if not isinstance(size, int) else size
        u = self.random((n, 12)).sum(axis=1) - 6.0
        return (mu + sigma * u).reshape(size)

    def choice(self, k, size, p):
        cdf = np.cumsum(np.asarray(p, dtype=np.float64))
        cdf /= cdf[-1]
        return np.minimum(np.searchsorted(cdf, self.random(size), side='right'), k - 1)

    def integers(self, lo, hi, size):
        return np.array([lo + v % (hi - lo) for v in self._u64(size)], dtype=np.int64)


def _q(e):
    """scores rounded (1e-6): last-bit differences between machines' maths libraries never pick another patch"""
    return np.round(np.asarray(e, dtype=np.float64), 6)


def fit(pcm, loop, f0, fplay, rng=None, rounds=5, batch=384):
    """-> (alg, fb, mul[4], tl[4], err); fplay = a typical played frequency (feedback depends on it)"""
    rng = rng or Rng(1)
    target = target_spectrum(pcm, loop, f0)
    hif = max(NH / 2 + 1, min(64, 12000.0 / max(fplay, 1)))      # (harmonics up to 12 kHz as played)
    best = []
    for alg in range(8):
        mods, car = ALG[alg]
        for fb in (0, 2, 4, 5, 6):            # (7: chaotic, rate-sensitive)
            C = batch // 4
            mi = rng.choice(len(MULS), size=(C, 4), p=MUL_W)
            for c in car[:1]: mi[:, c] = 1                              # (first carrier on the fundamental)
            mul = MULS[mi]
            tl = rng.uniform(0, 50, size=(C, 4))
            for c in car: tl[:, c] = np.where(rng.random(C) < 0.5, rng.uniform(0, 14, size=C), 127)
            tl[:, car[0]] = 0
            # (seeds: the simple patches - a 2-op stack S1 -> S2 at several depths, or S1 alone, the rest off)
            if alg in (4, 6):
                k = 24; tl[:k] = 127; tl[:k, 1] = 0; tl[:k, 0] = np.linspace(8, 56, k)
                mi[:k] = 1; mi[:k:3, 0] = 2; mul = MULS[mi]
            elif alg == 7:
                k = 6; tl[:k] = 127; tl[:k, 0] = 0; mi[:k] = 1; mul = MULS[mi]
            e = _q(distance(simulate(alg, fb, mul, tl, fplay, hif=hif), target, fplay))
            j = np.argsort(e, kind='stable')[:4]
            for q in j: best.append((e[q], alg, fb, mul[q].copy(), tl[q].copy()))
    best.sort(key=lambda b: b[0]); best = best[:12]
    for r in range(rounds):
        nb = []
        for (e0, alg, fb, mul, tl) in best[:6]:
            mods, car = ALG[alg]
            C = batch // 6
            m2 = np.repeat(mul[None], C, 0); t2 = np.repeat(tl[None], C, 0) + rng.normal(0, 6.0 / (r + 1), (C, 4))
            sw = rng.random((C, 4)) < 0.15
            m2[sw] = MULS[rng.choice(len(MULS), size=sw.sum(), p=MUL_W)]
            for c in car[:1]: m2[:, c] = 1
            t2 = np.clip(t2, 0, 100); t2[:, car[0]] = 0
            fbs = np.clip(fb + rng.integers(-1, 2, C), 0, 6)
            for fbv in np.unique(fbs):
                sel = fbs == fbv
                e = _q(distance(simulate(alg, int(fbv), m2[sel], t2[sel], fplay, hif=hif), target, fplay))
                for q in np.argsort(e, kind='stable')[:3]: nb.append((e[q], alg, int(fbv), m2[sel][q].copy(), t2[sel][q].copy()))
        best = sorted(best + nb, key=lambda b: b[0])[:12]
    e, alg, fb, mul, tl = best[0]
    return alg, fb, [float(m) for m in mul], [int(round(x)) for x in tl], float(e), target

def mul_reg(m): return 0 if m == 0.5 else int(m)
