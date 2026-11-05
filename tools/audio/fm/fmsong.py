#!/usr/bin/env python3
"""(I24) SOUND FM, step 2: one song -> the Z80 driver's command stream (symbolic), its instruments and
its PCM sounds.  Analysis helpers (samples, patches, envelopes) are fmconv.py's.

Time base: the sound driver's own tick (4 x $EC samples at 32 kHz; $EC = 100 -> 12.5 ms, 80 Hz) - the
Z80 waits in these ticks (FM_TICK_LEN DAC samples each).

The Z80's resources (z80/fmdrv.asm):
  FM 0-4      YM2612 channels 1-5 (channel 6 = the DAC)
  PCM 0-2     3 sounds mixed into the DAC at PCM_RATE: each a build-time "variant" = a sample at one pitch,
              its ADSR and its level baked in (signed 8-bit; the mixer only adds - the build makes sure the
              sum never leaves -128..127)
  PSG 0-2     tones (overflow melodic voices), PSG noise: hi-hats / cymbals
Loops: the song's repeating part found from the engine's key-ons (per voice: random ambience voices
never repeat); the stream = intro + body, the body's end jumps to its start.  Notes crossing the loop
point are cut there (the point is picked where the fewest notes cross)."""
import collections, math, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fmconv, fmfit
from fmconv import RATE_PERIOD, EXP_DB, SLOT, adsr_to_ym, fnum_block, sample_info, patch_for, s8

TICK32 = 400                                 # engine tick in 32 kHz samples ($EC = 100)
YM_CLK = 7670453.0
PCM_RATE = YM_CLK / 144.0 / 4.0              # YM timer A = 1020: 13316.76 Hz
TICK_LEN = TICK32 / 32000.0 * PCM_RATE       # DAC samples a tick (166.46)
N_FM, N_PCM = 6, 0                          # (I24 design: no DAC - channel 6 is FM too)
FM_FULL, PSG_RMS = fmconv.FM_FULL, fmconv.PSG_RMS
PSG_CLK = fmconv.PSG_CLK
VOL_DB = 3.0                                 # PCM variant level steps
AMB_PITCHES = 3                              # a non-melodic sample played at more pitches = ambience (32X)
GHOST_DULL = 8                               # (the echo's FIR: modulators 8 TL steps lower)


# ------------------------------------------------------------------------------------------- log -> notes
def grid_offset(tlog):
    """the engine's tick grid: t32 = GRID + TICK32 x k after its first ticks (at $EC = 60)"""
    return tlog[-1][0] % TICK32


def events_from_ticks(tlog):
    """[(t32, writes)] -> [(tick, reg, val)]: tick k = 32 kHz time grid_offset + TICK32 x k (the first ticks,
    before the grid, at 0)"""
    off = grid_offset(tlog)
    ev = []
    for t32, ws in tlog:
        k = max(0, int(round((t32 - off) / float(TICK32))))
        for r, v in ws: ev.append((k, r, v))
    return ev


def voice_tokens(ev):
    """per voice: [(tick, (srcn, pitch))] of its key-ons"""
    regs = [0] * 128
    out = [[] for _ in range(8)]
    for k, r, v in ev:
        regs[r] = v
        if r == 0x4C:
            for i in range(8):
                if v >> i & 1:
                    out[i].append((k, (regs[i * 16 + 4], regs[i * 16 + 2], regs[i * 16 + 3] & 0x3F)))
    return out


def find_loop(ev, nticks):
    """-> (loop start tick L, period P ticks) or None.  Each voice's key-on sequence: the shortest period
    it repeats with to the end of the log (2+ periods); voices that never repeat (random ambience: their
    "period" is a chance match at the log's very end) are ignored.  The song's period = the longest; every
    periodic voice must fit it."""
    def per(toks):
        n = len(toks)
        for P in range(1, n // 3 + 1):
            i = n - P - 1
            while i >= 0 and toks[i] == toks[i + P]: i -= 1
            i0 = i + 1
            if n - P - i0 >= 2 * P: return i0, P
        return None
    res = []
    for vt in voice_tokens(ev):
        if len(vt) < 8: continue
        r = per([x[1] for x in vt])
        if not r: continue
        i0, P = r
        dk = collections.Counter(vt[i + P][0] - vt[i][0] for i in range(i0, len(vt) - P))
        Pk = dk.most_common(1)[0][0]
        if vt[i0][0] + 2 * Pk <= nticks - 40: res.append((Pk, vt[i0][0]))
    if not res: return None
    P = max(r[0] for r in res)
    if P < 80: return None                   # (under a second: a static tail, not music)
    res = [r for r in res if r[1] + 2 * P <= nticks - 40]
    if not res or max(r[0] for r in res) != P: return None
    # (a voice whose period does not divide the song's runs free - an ostinato: it is just recorded)
    res = [r for r in res if abs(P / float(r[0]) - round(P / float(r[0]))) <= 0.02]
    L = max(r[1] for r in res)
    if L + 2 * P > nticks: return None
    return L, P


def periodic_srcn(ev, loop):
    """the samples whose key-ons repeat with the song (same offsets in the period, same pitches): the others
    are random (ambience)"""
    out = set()
    if not loop: return None
    L, P = loop
    by = collections.defaultdict(lambda: (collections.Counter(), collections.Counter()))
    for vt in voice_tokens(ev):
        for k, (srcn, plo, phi) in vt:
            if L <= k < L + P: by[srcn][0][((k - L), plo, phi)] += 1
            elif L + P <= k < L + 2 * P: by[srcn][1][((k - L - P), plo, phi)] += 1
    for srcn, (c1, c2) in by.items():
        # (a tick of jitter: the tempo accumulator)
        m = sum((c1 & c2).values())
        m2 = sum((c1 & collections.Counter({(k + 1, a, b): v for (k, a, b), v in c2.items()})).values())
        m3 = sum((c1 & collections.Counter({(k - 1, a, b): v for (k, a, b), v in c2.items()})).values())
        tot = max(sum(c1.values()), sum(c2.values()), 1)
        if (m + m2 + m3) >= 0.9 * tot: out.add(srcn)
    return out


def song_end(ev):
    """the tick after which nothing keys on (ended songs: jingles)"""
    last = 0
    for k, r, v in ev:
        if r == 0x4C and v: last = k
    return last


# ------------------------------------------------------------------------------------------- the song
class Song:
    def __init__(self):
        self.inst = []                       # instruments (patch_regs)
        self.inst_idx = {}
        self.env = []                        # envelopes (env_regs)
        self.env_idx = {}
        self.var = []                        # PCM variants: (np.int8 array, loop offset or None)
        self.var_idx = {}
        self.cmds = []                       # (tick, seq, cmd tuple)
        self.loop = None                     # tick of the loop start in the stream (None = ends)
        self.end = 0                         # tick of the end (loop jump / END)
        self.stats = collections.Counter()
        self.gain = 1.0

    def inst_id(self, regs):
        if regs not in self.inst_idx:
            self.inst_idx[regs] = len(self.inst)
            self.inst.append(regs)
        return self.inst_idx[regs]

    def env_id(self, regs):
        if regs not in self.env_idx:
            self.env_idx[regs] = len(self.env)
            self.env.append(regs)
        return self.env_idx[regs]


def patch_regs(P):
    """an instrument (the patch; the envelope is a KEYON's own): (b0, carrier mask, follow, ((mul, tl) x 4))
    follow = 1: the modulators take the note's envelope too (a non-looping sample's brightness fades)"""
    alg = P['alg']; car = fmfit.ALG[alg][1]
    ops = tuple((fmfit.mul_reg(P['mul'][k]), int(max(0, min(127, P['tl'][k])))) for k in range(4))
    return (P['fb'] << 3 | alg, sum(1 << k for k in car), 0 if P.get('looped', True) else 1, ops)


def env_regs(ar, d1r, d1l, d2r, ramp, ghost):
    """a note's envelope: (50, 60, 70, 80 for the carriers, post-ramp 60, 70, dull) - ramp: AR 31 and no decay
    until ENV2; dull (an echo ghost): modulators 8 TL steps quieter, a slower release"""
    rr = 9 if ghost else 13
    return (31 if ramp else ar, 0 if ramp else d1r, 0 if ramp else d2r, d1l << 4 | rr, d1r, d2r, GHOST_DULL if ghost else 0)


def fit_requests(pack, smap, ev, cache):
    """the FM patch fits this song asks for (convert's first part): {pack sample: its playing frequency} -
    fmbuild.py fits each sample once, at the frequency of the first song (in pack order) that plays it"""
    notes = fmconv.notes_of(ev)
    used = collections.defaultdict(list)
    for n in notes: used[n['srcn']].append(n['track'][0][1])
    req = {}
    for s, ps in used.items():
        if s not in smap: continue
        gi = smap[s]
        inf = sample_info(pack, gi, cache.setdefault('info', {}))
        tonal = inf['f0'] > 0 and (inf['looped'] or inf['flat'] < 0.3)
        if not tonal or (not inf['looped'] and len(set(ps)) <= 2) or inf['n'] < 64 and not inf['looped']:
            continue
        req[gi] = round(inf['f0'] * float(np.median(ps)) / 4096.0, 9)
    return req


def convert(pack, smap, ev, nticks, cache, loop=None, only=0xFF, echo=True):
    """-> Song.  ev: [(tick, reg, val)]"""
    notes = fmconv.notes_of(ev)
    S = Song()
    periodic = periodic_srcn(ev, loop)
    # ---- instruments / roles (fmconv's analysis)
    used = collections.defaultdict(list)
    for n in notes: used[n['srcn']].append(n['track'][0][1])
    role, info, patch = {}, {}, {}
    for s, ps in used.items():
        if s not in smap: role[s] = 'skip'; continue
        gi = smap[s]
        inf = sample_info(pack, gi, cache.setdefault('info', {}))
        info[s] = inf
        pitches = len(set(ps))
        tonal = inf['f0'] > 0 and (inf['looped'] or inf['flat'] < 0.3)
        if not tonal or (not inf['looped'] and pitches <= 2) or inf['n'] < 64 and not inf['looped']:
            # (drums, hi-hats, ambience: the 32X plays them - the S-DSP voice is mixed there from the game's
            #  own sample as in SOUND FULL, echo included; a DAC variant per pitch and level would not fit
            #  the ROM the Mega Drive side can see)
            role[s] = 'spc'; continue
        fplay = round(inf['f0'] * float(np.median(ps)) / 4096.0, 9)
        p = dict(patch_for(pack, gi, inf, fplay, cache.setdefault('patch', {})))
        p['looped'] = inf['looped']
        patch[s] = p
        role[s] = 'fm' if p['err'] < 150 else 'spc'
    S.role, S.info, S.patch = role, info, patch
    G = 1.0
    S.gain = G

    def level_of(s, vl, vr, mv):
        return G * 2 * info[s]['rms'] * 16384.0 * max(abs(vl), abs(vr)) / 128.0 * mv / 128.0

    # ---- echo ghosts (EDL later, EVOL quieter; FM: duller; PCM: on an idle channel)
    work = []
    for n in notes:
        if not (only >> n['voice'] & 1): continue
        work.append(n)
        if echo and n['echo'] and n['edl'] > 0 and n['evol'] > 0.08 and role.get(n['srcn']) == 'fm':
            g = dict(n); g['ghost'] = n['evol']
            d = int(round(n['edl'] * 32000 / TICK32))
            if d < 1: continue
            g['t0'] = n['t0'] + d; g['t1'] = n['t1'] + d
            g['track'] = [(e[0] + d,) + tuple(e[1:]) for e in n['track']]
            work.append(g)
    work.sort(key=lambda n: (n['t0'], 1 if n.get('ghost') else 0))
    # ---- segments: intro [0, L) and body [L, L + P) - or the whole song
    if loop:
        L, P = loop
        # (the loop point: where the fewest notes sound across it - they are cut there)
        best = None
        for k in range(L, L + P):
            c = sum(1 for n in notes if n['t0'] < k < n['t1'] and not n.get('ghost'))
            if best is None or c < best[0]: best = (c, k)
            if c == 0: break
        L = best[1]
        S.stats['loop cut notes'] = best[0]
        segs = [(0, L), (L, L + P)]
        S.loop, S.end = L, L + P
    else:
        e = song_end(ev)
        end = min(nticks, max([n['t1'] for n in notes if n['t0'] <= e] + [e]) + 80)
        segs = [(0, end)]
        S.loop, S.end = None, end
    seq = [0]
    def emit(L_, t, *c):
        seq[0] += 1
        L_.append((t, seq[0], c))
    lists = []
    pcm_notes = []                           # (seg index, ch, k0, k1, note, s, ghost)
    # (each S-DSP voice that plays FM notes gets a channel of its own when there are enough: a phrase then
    #  encodes the same bytes every time it repeats - the stream's compression finds it)
    use = collections.Counter(n['voice'] for n in notes if role.get(n['srcn']) == 'fm')
    pref = {}
    for v, _ in use.most_common():
        if len(pref) < N_FM: pref[v] = len(pref)
    S.pref = pref
    for si, (a, b) in enumerate(segs):
        fm_end = [0] * N_FM; fm_last = [None] * N_FM; fm_lvl = [0.0] * N_FM; fm_ghost = [False] * N_FM
        owner = [None] * N_FM
        psg_end = [0] * 3; pcm_end = [0] * N_PCM; pcm_ghost = [False] * N_PCM; pcm_own = [None] * N_PCM
        noise_own = [None]
        def cancel(ch, T):
            if owner[ch] is None: return
            Lc = owner[ch]
            Lc[:] = [e for e in Lc if e[0] < T]
            emit(Lc, T, 'keyoff', ch)
            owner[ch] = None
        for n in work:
            k0 = n['t0']
            if not (a <= k0 < b): continue
            s = n['srcn']; r = role.get(s, 'skip')
            ghost = n.get('ghost')
            if r == 'skip': S.stats['skip'] += 1; continue
            if r == 'spc': S.stats['32X'] += 1; continue
            T0, T1 = k0, min(max(n['t1'], k0 + 1), b)
            t_, p, vl, vr, mv = n['track'][0]
            lvl = level_of(s, vl, vr, mv) * (ghost or 1.0)
            if lvl <= 0.5: S.stats['silent'] += 1; continue
            inf = info[s]
            target = r
            ch = None
            if r == 'fm':
                free = [c for c in range(N_FM) if fm_end[c] <= T0]
                if not free and not ghost:
                    free = [c for c in range(N_FM) if fm_ghost[c]]
                if free:
                    pv = pref.get(n['voice'])
                    if not ghost and pv is not None and pv in free: ch = pv
                    else:
                        # (ghosts / other voices: a channel no voice prefers first, then the one idle longest)
                        own = set(pref.values())
                        cand = [c for c in free if c not in own] or free
                        same = [c for c in cand if fm_last[c] == s]
                        ch = (same or sorted(cand, key=lambda c: fm_end[c]))[0]
                    if fm_end[ch] > T0: cancel(ch, T0); S.stats['ghost cut'] += 1
                elif ghost:
                    S.stats['ghost dropped'] += 1; continue
                if ch is None:
                    if True:
                        sc = next((c for c in range(3) if psg_end[c] <= T0), None)
                        if sc is not None: target = 'psg'
                        else:
                            qc = min(range(N_FM), key=lambda c: fm_lvl[c])
                            if fm_lvl[qc] < lvl: ch = qc; cancel(ch, T0); S.stats['fm steal'] += 1
                            else: S.stats['dropped'] += 1; continue
            if target == 'noise':
                if ghost: continue
                Lc = []; lists.append(Lc)
                ratio = p / 4096.0
                dur = min(T1 - T0, int(math.ceil(inf['n'] / (32000.0 * ratio) * 32000 / TICK32)))
                rate = 0 if inf['centroid'] * ratio > 7000 else 1
                if noise_own[0] is not None:
                    noise_own[0][:] = [e for e in noise_own[0] if e[0] < T0]
                noise_own[0] = Lc
                emit(Lc, T0, 'psg', (0xE0 | 4 | rate,))
                sdec = inf['decay'] * ratio
                last = None
                for i in range(dur):
                    tt = i * TICK32 / 32000.0
                    db = float(env_db(n, np.array([tt]))[0]) + sdec * tt
                    av = lvl * 10 ** (-db / 20.0)
                    att = int(max(0, min(15, round(20 * math.log10(PSG_RMS / max(av, 1e-3)) / 2.0))))
                    if att != last: emit(Lc, T0 + i, 'psg', (0xF0 | att,)); last = att
                    if att == 15 and i > 0: break
                emit(Lc, min(T0 + dur, b), 'psg', (0xFF,))
                S.stats['noise'] += 1
                continue
            if target == 'pcm':
                pc = next((c for c in range(N_PCM) if pcm_end[c] <= T0), None)
                if pc is None and ghost: S.stats['ghost dropped'] += 1; continue
                if pc is None: pc = next((c for c in range(N_PCM) if pcm_ghost[c]), None)
                if pc is None: pc = min(range(N_PCM), key=lambda c: pcm_end[c]); S.stats['pcm steal'] += 1
                if ghost: T1 = min(b, T1 + int(n['edl'] * 32000 / TICK32 * max(0, min(3, abs(n['efb']) * 4))))
                pn = dict(seg=si, ch=pc, k0=T0, k1=T1, note=n, s=s, gscale=ghost or 1.0)
                pcm_notes.append(pn)
                # (the sound's own end: tails keep the channel; the variant decides - see below)
                ratio = p / 4096.0
                L_len = T1 if inf['looped'] else T0 + int(math.ceil(inf['n'] / (32000.0 * ratio) * 32000 / TICK32))
                pcm_end[pc] = max(T1, min(b, L_len)) if not inf['looped'] else T1
                pcm_ghost[pc] = bool(ghost)
                S.stats[('pcm ghost' if ghost else 'pcm') + (' (overflow)' if r == 'fm' else '')] += 1
                continue
            Lc = []; lists.append(Lc)
            if target == 'psg':
                sc = next(c for c in range(3) if psg_end[c] <= T0)
                psg_end[sc] = T1 + 1; S.stats['psg'] += 1
                last = None
                for (tt, p2, vl2, vr2, mv2) in n['track']:
                    T = max(tt, T0)
                    if T >= T1: break
                    freq = inf['f0'] * p2 / 4096.0
                    while 0 < freq and freq * 32 * 1023 < PSG_CLK: freq *= 2
                    N = max(1, min(1023, int(round(PSG_CLK / (32 * max(freq, 1.0))))))
                    av = level_of(s, vl2, vr2, mv2)
                    att = int(max(0, min(15, round(20 * math.log10(PSG_RMS / max(av, 1e-3)) / 2.0))))
                    if (N, att) != last:
                        emit(Lc, T, 'psg', (0x80 | sc << 5 | (N & 15), N >> 4, 0x90 | sc << 5 | att)); last = (N, att)
                emit(Lc, T1, 'psg', (0x90 | sc << 5 | 15,))
                continue
            # ---- FM
            S.stats['fm ghost' if ghost else 'fm'] += 1
            P_ = patch[s]
            alg = P_['alg']; car = fmfit.ALG[alg][1]
            ratio = p / 4096.0
            blk0, _ = fnum_block(inf['f0'] * ratio)
            ar, d1r, d1l, d2r = adsr_to_ym(n['adsr1'], n['adsr2'], n['gain'], 0 if inf['looped'] else inf['decay'], ratio)
            # (no per-pitch rate correction: one instrument for every pitch of a sample - the YM's own
            #  key scaling is small at KS 0)
            t_att = 0.0
            if n['adsr1'] & 0x80 and (n['adsr1'] & 15) < 15:
                t_att = 64 * RATE_PERIOD[(n['adsr1'] & 15) * 2 + 1] / 32000.0
            ramp = t_att > 0.03
            if not inf['looped']:
                Tend = T0 + int(math.ceil(inf['n'] / (32000.0 * ratio) * 32000 / TICK32))
                T1 = min(T1, max(Tend, T0 + 1))
            if ghost:
                T1 = min(b, T1 + int(n['edl'] * 32000 / TICK32 * max(0, min(3, abs(n['efb']) * 4))))
            fm_end[ch] = T1 + 1; fm_last[ch] = s; fm_lvl[ch] = lvl; fm_ghost[ch] = bool(ghost)
            owner[ch] = Lc
            iid = S.inst_id(patch_regs(P_))
            eid = S.env_id(env_regs(ar, d1r, d1l, d2r, ramp, bool(ghost)))
            gsc = ghost or 1.0
            def tl_car(av):
                return 20 * math.log10(max(P_['rms'], 1e-6) * FM_FULL / max(av * gsc, 1e-3)) / 0.75
            Tr = T0 + int(round(t_att * 32000 / TICK32)) if ramp else T0
            pts = sorted(set([max(e[0], T0) for e in n['track']] + (list(range(T0, Tr + 1, 2)) if ramp else [])))
            ti = 0
            lastf = lastv = lastpan = None
            first = True
            for T in pts:
                if T >= T1: break
                while ti + 1 < len(n['track']) and max(n['track'][ti + 1][0], T0) <= T: ti += 1
                (tt, p2, vl2, vr2, mv2) = n['track'][ti]
                L_, R_ = abs(vl2), abs(vr2); m = max(L_, R_, 1)
                pan = (2 if L_ > 0.35 * m else 0) | (1 if R_ > 0.35 * m else 0) or 3
                bf = fnum_block(inf['f0'] * p2 / 4096.0)
                fn = bf[0] << 11 | bf[1]
                v = tl_car(level_of(s, vl2, vr2, mv2))
                if ramp and T < Tr:
                    v += -20 * math.log10(max((T - T0) / float(max(Tr - T0, 1)), 1e-3)) / 0.75
                vq = int(max(0, min(127, round(v))))
                if first:
                    emit(Lc, T, 'keyon', ch, iid, eid, fn, vq, pan)
                    lastf, lastv, lastpan, first = fn, vq, pan, False
                    continue
                if pan != lastpan: emit(Lc, T, 'pan', ch, pan); lastpan = pan
                if fn != lastf: emit(Lc, T, 'freq', ch, fn); lastf = fn
                if vq != lastv: emit(Lc, T, 'vol', ch, vq); lastv = vq
                if ramp and T == Tr: emit(Lc, T, 'env2', ch)
            if ramp and Tr < T1 and Tr not in pts: emit(Lc, Tr, 'env2', ch)
            emit(Lc, T1, 'keyoff', ch)
        # (the segment's end: whatever still sounds is cut - the loop point was picked to need ~none)
        for ch in range(N_FM):
            if owner[ch] is not None and fm_end[ch] > b:
                cancel(ch, b - 1 if b > a else b)
    # ---- PCM: variants (sample at the note's pitch, ADSR + level baked), the mix kept inside 8 bits
    for L_ in lists: S.cmds.extend(L_)
    S.cmds.sort(key=lambda e: (e[0], e[1]))
    S.segs = segs
    return S


def env_db(n, tt):
    """the S-DSP ADSR (dB below full) at times tt (s) after key-on"""
    a = n['adsr1']
    if not (a & 0x80): return np.zeros_like(tt)
    ar_ = a & 15
    t_att = 2 / 32000.0 if ar_ == 15 else 64 * RATE_PERIOD[ar_ * 2 + 1] / 32000.0
    d_dbs = EXP_DB * 32000.0 / RATE_PERIOD[((a >> 4) & 7) * 2 + 16]
    sl_db = -20 * math.log10(((n['adsr2'] >> 5) + 1) / 8.0)
    sr = n['adsr2'] & 31; s_dbs = EXP_DB * 32000.0 / RATE_PERIOD[sr] if sr else 0.0
    att = -20 * np.log10(np.maximum(1e-3, np.minimum(1.0, tt / max(t_att, 1e-6))))
    dd = np.minimum(sl_db, np.maximum(0, tt - t_att) * d_dbs)
    t_s = t_att + (sl_db / d_dbs if d_dbs else 1e9)
    return att + dd + np.maximum(0, tt - t_s) * s_dbs


MAX_VAR = 2.0          # s: the longest non-looping variant (a looping one: head + one loop)


def make_variant(pack, gi, inf, n, vstep, gscale, G):
    """the sound at this note's pitch, 8-bit at PCM_RATE: (np.int8 data, loop offset or None)"""
    pcm, loop = pack.sample(gi)
    t, p, vl, vr, mv = n['track'][0]
    ratio = p / 4096.0
    f = PCM_RATE / (32000.0 * ratio)          # (output samples per source sample)
    src = pcm
    if f < 1.0 and len(pcm) > 64:
        from scipy.signal import firwin, filtfilt
        src = filtfilt(firwin(31, 0.9 * f), [1.0], pcm)
    L = len(pcm)
    looped = inf['looped'] and L - loop >= 8
    if looped:
        # (head = up to the loop's start + the envelope's decay; then a whole number of loop passes)
        a = n['adsr1']
        t_dec = 0.0
        if a & 0x80:
            ar_ = a & 15
            t_att = 2 / 32000.0 if ar_ == 15 else 64 * RATE_PERIOD[ar_ * 2 + 1] / 32000.0
            d_dbs = EXP_DB * 32000.0 / RATE_PERIOD[((a >> 4) & 7) * 2 + 16]
            sl_db = -20 * math.log10(((n['adsr2'] >> 5) + 1) / 8.0)
            t_dec = t_att + (sl_db / d_dbs if d_dbs else 0)
        lp_out = (L - loop) * f
        n_head = max(loop * f, t_dec * PCM_RATE)
        k = int(math.ceil((n_head - loop * f) / lp_out)) if n_head > loop * f else 0
        n_head = loop * f + k * lp_out
        # (one loop pass in output samples, rounded: the pitch error of the rounding < 1 / length)
        n_lp = max(8, int(round(lp_out)))
        nout = int(round(n_head)) + n_lp
        pos = np.arange(nout) / f
        over = pos >= L
        pos[over] = loop + (pos[over] - loop) % (L - loop)
        y = np.interp(pos, np.arange(L), src)
        lp_off = int(round(n_head))
    else:
        nout = min(int(L * f), int(MAX_VAR * PCM_RATE))
        y = np.interp(np.arange(nout) / f, np.arange(L), src)
        lp_off = None
    tt = np.arange(len(y)) / PCM_RATE
    env = 10 ** (-env_db(n, tt) / 20.0)
    if looped:
        env[lp_off:] = env[lp_off]           # (the loop holds the sustain level; SR: VOL steps - none here)
    amp = 10 ** (-vstep * VOL_DB / 20.0)     # (vstep 0 = full scale)
    y8 = np.clip(np.round(y * env * amp * 127.0 / max(np.abs(y).max(), 1e-6) * 1.0), -127, 127).astype(np.int8)
    if not looped:
        nz = np.nonzero(y8)[0]
        y8 = y8[:nz[-1] + 1] if len(nz) else y8[:1]
    return y8, lp_off


def pcm_variants(S, pack, smap, pcm_notes, segs, level_of, info):
    """variants + commands; the mixer has no saturation: the three channels' sum must stay in 8 bits -
    simulated here, the louder notes of an overlap one step quieter until it does"""
    cmds = []
    for pn in pcm_notes:
        n, s = pn['note'], pn['s']
        inf = info[s]
        t, p, vl, vr, mv = n['track'][0]
        pk = inf['peak']
        amp = S.gain * 2 * 16384.0 * pk / 64.0 * max(abs(vl), abs(vr)) / 128.0 * mv / 128.0 * pn['gscale']
        # (the variant is normalized to its own peak: the level = amp / 127 of full scale, in VOL_DB steps)
        pn['vstep'] = int(max(0, round(-20 * math.log10(max(amp / 127.0, 1e-4)) / VOL_DB)))
        pn['peak8'] = 127.0 * 10 ** (-pn['vstep'] * VOL_DB / 20.0)
    def build(pn):
        n, s = pn['note'], pn['s']
        key = (smap[s], n['track'][0][1], n['adsr1'], n['adsr2'], n['gain'], pn['vstep'])
        if key not in S.var_idx:
            y8, lp = make_variant(pack, smap[s], info[s], n, pn['vstep'], pn['gscale'], S.gain)
            if pn['vstep'] * VOL_DB > 40:
                y8 = np.zeros(1, np.int8); lp = None
            S.var_idx[key] = len(S.var)
            S.var.append((y8, lp))
        pn['var'] = S.var_idx[key]
    # mix check per segment, at DAC-sample resolution
    for it in range(12):
        for pn in pcm_notes: build(pn)
        bad = set()
        for si, (a, b) in enumerate(segs):
            ns = [pn for pn in pcm_notes if pn['seg'] == si]
            if not ns: continue
            n0 = int(a * TICK_LEN); n1 = int(b * TICK_LEN) + 1
            mix = np.zeros(n1 - n0, np.int32)
            spans = []
            for pn in ns:
                y8, lp = S.var[pn['var']]
                i0 = int(pn['k0'] * TICK_LEN) - n0
                i1 = min(n1 - n0, int(pn['k1'] * TICK_LEN) - n0)
                if lp is not None:
                    m = i1 - i0
                    body = y8[lp:] if len(y8) > lp else y8[-1:]
                    rep = np.concatenate([y8[:lp], np.tile(body, max(1, (m - lp) // max(len(body), 1) + 2))])[:max(0, m)]
                    seg = rep
                else:
                    seg = y8[:max(0, i1 - i0)]
                spans.append((pn, i0, i0 + len(seg), seg))
            # (a channel's newer note stops its older one: the spans are cut at the next start on that channel)
            spans.sort(key=lambda x: x[1])
            last = {}
            cut = []
            for sp in spans:
                pn, i0, i1, seg = sp
                c = pn['ch']
                if c in last:
                    j = last[c]
                    p0, a0, b0, sg0 = cut[j]
                    if b0 > i0: cut[j] = (p0, a0, i0, sg0[:max(0, i0 - a0)])
                last[c] = len(cut)
                cut.append(sp)
            for pn, i0, i1, seg in cut:
                if i1 > i0: mix[i0:i1] += seg[:i1 - i0].astype(np.int32)
            over = np.nonzero(np.abs(mix) > 127)[0]
            if len(over):
                for pn, i0, i1, seg in cut:
                    if i1 > i0 and np.any((over >= i0) & (over < i1)): bad.add(id(pn))
        if not bad: break
        S.stats['pcm overflow fixes'] += len(bad)
        for pn in pcm_notes:
            if id(pn) in bad: pn['vstep'] += 1
    nxt = {}
    for pn in sorted(pcm_notes, key=lambda x: -x['k0']):
        pn['next'] = nxt.get((pn['seg'], pn['ch']), 1 << 30)
        nxt[(pn['seg'], pn['ch'])] = pn['k0']
    for pn in pcm_notes:
        y8, lp = S.var[pn['var']]
        cmds.append((pn['k0'], 1, ('pcm', pn['ch'], pn['var'])))
        nat = pn['k0'] + (len(y8) / TICK_LEN if lp is None else 1e9)
        if pn['k1'] < nat and pn['k1'] < pn['next']:
            cmds.append((pn['k1'], 0, ('pcmstop', pn['ch'])))
    S.pcm_notes = pcm_notes
    return cmds
