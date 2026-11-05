#!/usr/bin/env python3
"""(I24) SOUND FM: the game's music for the Mega Drive's own sound chips, converted at build time from the
DSP register writes of the game's sound driver (tools/audio/fmdump.c).  Nothing of the music ships: the
builder makes this from the user's ROM.

Targets (the Z80 driver's model):
  YM2612 FM 1-5   melodic voices; each sample gets a 4-op patch fitted to its spectrum (fmfit.py),
                  the SNES ADSR becomes the carriers' envelope, volume -> TL, pan -> L/R
  YM2612 DAC      PCM_CH channels mixed (8-bit, PCM_RATE): drums / ambience / overflow voices, the game's
                  own samples pre-pitched and pre-levelled at build time (XGM-style fixed-rate mixing)
  SN76489 1-3     overflow melodic voices (square)

v1 output: an event list for the preview renderer (t p r v at 44.1 kHz) + the mixed DAC stream."""
import collections, json, math, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from samples import Pack, features, f0_pick
import fmfit

OUT_RATE = 44100
PCM_RATE = 13300.0
PCM_CH = 4
N_FM = 5
YM_CLK = 7670453.0
PSG_CLK = 3579545.0
FM_FULL = 8168.0             # one carrier at TL 0 (renderer units)
PSG_RMS = 2304.0             # one tone at attenuation 0
SLOT = (0, 8, 4, 12)         # register offset of S1..S4
RATE_PERIOD = [0, 2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256, 192, 160, 128, 96, 80, 64, 48, 40, 32, 24,
               20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1]
EXP_DB = 20 * math.log10(256 / 255.0)    # S-DSP exponential step (dB)

# ------------------------------------------------------------------------------------------- the log
def parse(log):
    smap, ev = {}, []
    for l in open(log):
        if l[0] == '#':
            a, b = l[1:].split(); smap[int(a)] = int(b); continue
        t, r, v = l.split(); ev.append((int(t), int(r, 16), int(v, 16)))
    return smap, ev

def s8(v): return (v ^ 128) - 128

def notes_of(ev):
    """notes: dict(t0, t1, srcn, voice, adsr1, adsr2, gain, track=[(t, pitch, volL, volR, mvol)])"""
    regs = [0] * 128
    on = [None] * 8
    out = []
    def pt(i, t): return (t, (regs[i * 16 + 2] | regs[i * 16 + 3] << 8) & 0x3FFF, s8(regs[i * 16]), s8(regs[i * 16 + 1]),
                          max(abs(s8(regs[0x0C])), abs(s8(regs[0x1C]))))
    def close(i, t):
        on[i]['t1'] = t; out.append(on[i]); on[i] = None
    for t, r, v in ev:
        regs[r] = v
        vi, lo = r >> 4, r & 15
        if r == 0x4C:
            for i in range(8):
                if v >> i & 1:
                    if on[i]: close(i, t)
                    on[i] = dict(t0=t, srcn=regs[i * 16 + 4], voice=i, adsr1=regs[i * 16 + 5], adsr2=regs[i * 16 + 6],
                                 gain=regs[i * 16 + 7], track=[pt(i, t)], noise=bool(regs[0x3D] >> i & 1),
                                 echo=bool(regs[0x4D] >> i & 1) and not (regs[0x6C] & 0x20),
                                 evol=max(abs(s8(regs[0x2C])), abs(s8(regs[0x3C]))) / 128.0,
                                 efb=s8(regs[0x0D]) / 128.0, edl=(regs[0x7D] & 15) * 0.016)
        elif r == 0x5C:
            for i in range(8):
                if v >> i & 1 and on[i]: close(i, t)
        elif vi < 8 and lo in (0, 1, 2, 3) and on[vi]:
            on[vi]['track'].append(pt(vi, t))
        elif r in (0x0C, 0x1C):
            for i in range(8):
                if on[i]: on[i]['track'].append(pt(i, t))
    for i in range(8):
        if on[i]: close(i, ev[-1][0])
    out.sort(key=lambda n: n['t0'])
    return out

def detect_tick(notes):
    """the sound driver's tick (32 kHz samples) and phase: the grid the key-ons sit on"""
    ts = np.array(sorted(set(n['t0'] for n in notes)), float)
    best = (1e9, 400.0, 0.0)
    for T in np.arange(160.0, 1100.0, 1.0):
        ph = np.angle(np.exp(2j * np.pi * ts / T).mean()) / (2 * np.pi) * T
        d = np.abs(((ts - ph) / T + 0.5) % 1.0 - 0.5) * T
        sc = float(np.mean(np.minimum(d, 40.0))) + T * 0.002     # (prefer the shorter tick on ties: none)
        if sc < best[0]: best = (sc, T, ph)
    return best[1], best[2], best[0]

# ------------------------------------------------------------------------------------------- samples
def refine_f0(f, pcm, loop):
    """autocorrelation estimate, snapped to the loop (a loop holds a whole number of periods)"""
    f0 = f['f0']
    if f0 <= 0: return 0.0
    if f['looped'] and f['n'] - loop >= 8:
        L = f['n'] - loop
        if L <= 4096:
            k = max(1, round(L * f0 / 32000.0)); f0 = 32000.0 * k / L
    return f0

def sample_info(pack, gi, cache):
    key = str(gi)
    if key in cache: return cache[key]
    pcm, loop = pack.sample(gi)
    f = features(pcm, loop)
    f0 = f0_pick(pcm, loop, f['f0'])
    n = len(pcm); looped = loop < n
    body = pcm[loop:] if looped and n - loop >= 16 else pcm[:min(n, 640)]
    rms = float(np.sqrt(np.mean(body ** 2))) if len(body) else 0.0
    # the sample's own decay (non-looped): dB/s at native pitch
    env = np.array([np.sqrt(np.mean(pcm[i:i + 256] ** 2)) for i in range(0, max(1, n - 255), 256)]) + 1e-6
    db = 20 * np.log10(env / env.max())
    i0 = int(np.argmax(db)); dec = 0.0
    if not looped and len(db) - i0 >= 3:
        t = np.arange(len(db) - i0) * 256 / 32000.0
        dec = float(-np.polyfit(t, db[i0:], 1)[0])
    info = dict(n=n, loop=loop, looped=looped, f0=f0, flat=f['flat'], centroid=f['centroid'], rms=rms,
                peak=f['peak'], decay=max(0.0, dec))
    # (I24: 9 decimals - the same numbers from every machine's maths library, so the same conversion)
    info = {k: (round(v, 9) if isinstance(v, float) else v) for k, v in info.items()}
    cache[key] = info
    return info

def patch_for(pack, gi, info, fplay, cache):
    key = '%d' % gi
    if key in cache: return cache[key]
    pcm, loop = pack.sample(gi)
    alg, fb, mul, tl, err, _ = fmfit.fit(pcm, loop, info['f0'], fplay)
    _, rms = fmfit.simulate(alg, fb, np.array([mul]), np.array([tl], float), fplay, rms=True)
    p = dict(alg=alg, fb=fb, mul=mul, tl=tl, err=err, rms=float(rms[0]))
    cache[key] = p
    return p

# ------------------------------------------------------------------------------------------- envelopes
def ym_rate_for_dbs(dbs):
    """YM decay rate register (D1R / D2R) for a decay of dbs dB/s (measured: 0.8 dB/s at 1, x2 per 2 steps)"""
    if dbs <= 0.3: return 0
    return int(max(1, min(31, round(1 + 2 * math.log2(dbs / 0.8)))))

def ym_ar_for_time(t):
    """YM attack rate for an attack of t seconds (measured: 2.45 s to 90 % at AR 4, x2 per 2 steps)"""
    if t < 0.004: return 31
    return int(max(1, min(31, round(4 - 2 * math.log2(0.9 * t / 2.45)))))

def adsr_to_ym(a1, a2, gain, sdecay, ratio):
    """S-DSP ADSR / GAIN (+ a non-looped sample's own decay, scaled by pitch ratio) -> (ar, d1r, d1l, d2r)"""
    if a1 & 0x80:
        ar = a1 & 15
        t_att = 2 / 32000.0 if ar == 15 else 64 * RATE_PERIOD[ar * 2 + 1] / 32000.0
        dr = (a1 >> 4) & 7
        d_dbs = EXP_DB * 32000.0 / RATE_PERIOD[dr * 2 + 16]
        sl = a2 >> 5
        sl_db = -20 * math.log10((sl + 1) / 8.0)
        sr = a2 & 31
        s_dbs = EXP_DB * 32000.0 / RATE_PERIOD[sr] if sr else 0.0
    else:
        t_att, d_dbs, sl_db, s_dbs = 0.0, 0.0, 0.0, 0.0
        if gain & 0x80:
            mode, rate = gain >> 5 & 3, gain & 31
            per = RATE_PERIOD[rate]
            if per:
                if mode == 0: s_dbs = 60.0 / (64 * per / 32000.0)           # linear down (approx.)
                elif mode == 1: s_dbs = EXP_DB * 32000.0 / per
                else: t_att = 64 * per / 32000.0
    sd = sdecay * ratio
    if sd > 0:          # the sample fades by itself: never slower than that
        d_dbs = max(d_dbs, sd); s_dbs = max(s_dbs, sd)
        if sl_db < 1: sl_db = 0
    ar = ym_ar_for_time(t_att)
    d1l = int(max(0, min(15, round(sl_db / 3.0))))
    d1r = ym_rate_for_dbs(d_dbs) if d1l else ym_rate_for_dbs(s_dbs)
    d2r = ym_rate_for_dbs(s_dbs)
    return ar, d1r, d1l, d2r

def fnum_block(freq):
    for blk in range(8):
        fn = int(round(freq * 144.0 * (1 << 21) / YM_CLK / (1 << blk)))
        if fn < 2048: return blk, max(1, fn)        # (rounded first: 2047.5 is the next block's 1024)
    return 7, 2047

# ------------------------------------------------------------------------------------------- convert
def convert(pack, log, seconds, cache, report=None):
    smap, ev = parse(log)
    notes = notes_of(ev)
    tick, phase, _ = detect_tick(notes)
    def q(t32):     # 32 kHz time -> the driver's tick grid -> 44.1 kHz
        k = round((t32 - phase) / tick)
        return int(round((k * tick + phase) / 32000.0 * OUT_RATE)) if k >= 0 else 0
    tick44 = tick / 32000.0 * OUT_RATE
    # instruments
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
            # (hi-hats / cymbals: noise played high - beyond what the DAC's rate can hold: the PSG's noise)
            if not inf['looped'] and inf['flat'] > 0.45 and inf['centroid'] * float(np.median(ps)) / 4096.0 > 4500:
                role[s] = 'noise'; continue
            role[s] = 'pcm'; continue
        fplay = inf['f0'] * float(np.median(ps)) / 4096.0
        p = patch_for(pack, gi, inf, fplay, cache.setdefault('patch', {}))
        patch[s] = p
        role[s] = 'fm' if p['err'] < 150 else 'pcm'     # (FM for every melodic instrument: the point of the mode)
    events = []
    def w(t, part, reg, val): events.append((int(t), part, reg, val & 0xFF))
    w(0, 0, 0x22, 0); w(0, 0, 0x27, 0); w(0, 0, 0x2B, 0x80); w(0, 1, 0xB6, 0xC0)
    for c in range(3): w(0, 2, 0, 0x90 | c << 5 | 15)
    w(0, 2, 0, 0xFF)
    fm_end = [0] * N_FM; fm_last = [None] * N_FM; fm_lvl = [0.0] * N_FM
    psg_end = [0] * 3; pcm_end = [0] * PCM_CH
    total = int(seconds * PCM_RATE)
    mixbuf = np.zeros((PCM_CH, total))
    variants = {}
    stats = collections.Counter()

    # (the mix's headroom: one DAC channel is only as loud as one FM channel, the SNES drums are louder - the
    #  whole song is scaled so that 95 % of the drum hits fit; the player's volume knob does the rest)
    need = []
    for n in notes:
        if role.get(n['srcn']) == 'pcm':
            t, p_, vl, vr, mv = n['track'][0]
            need.append(2 * 16384.0 * info[n['srcn']]['peak'] / 64.0 * max(abs(vl), abs(vr)) / 128.0 * mv / 128.0)
    G = min(1.0, 127.0 / float(np.percentile(need, 95))) if need else 1.0

    def level_of(n, s, vl, vr, mv):
        inf = info[s]
        return G * 2 * inf["rms"] * 16384.0 * max(abs(vl), abs(vr)) / 128.0 * mv / 128.0     # (x2: measured, the S-DSP mix)

    # ---- PCM (the Z80 mixer's model): every sample stored ONCE, 8-bit, resampled at build time to how it sounds
    # at its usual pitch; the mixer steps through it (16.16 step, nearest sample) for other pitches; a 16-level
    # volume per channel (2 dB steps), set every tick from the note's volume and ADSR; 4 channels summed + clipped
    store = {}
    def stored(gi, ratio_typ):
        if gi in store: return store[gi]
        pcm, loop = pack.sample(gi)
        f = PCM_RATE / (32000.0 * ratio_typ)              # (output samples per source sample at the usual pitch)
        src = pcm
        if f < 1.0:                                       # (band-limit first)
            from scipy.signal import firwin, filtfilt
            src = filtfilt(firwin(31, 0.9 * f), [1.0], pcm) if len(pcm) > 100 else pcm
        n_out = max(1, int(len(pcm) * f))
        y = np.interp(np.arange(n_out) / f, np.arange(len(pcm)), src)
        pk = max(np.abs(y).max(), 1e-6)
        y8 = np.clip(np.round(y / pk * 127), -127, 127)
        lp = int(loop * f) if loop < len(pcm) else None
        store[gi] = (y8, lp, pk, ratio_typ)
        return store[gi]
    typ_ratio = {s: float(np.median(ps)) / 4096.0 for s, ps in used.items()}
    VOL = np.array([0.0] + [10 ** (-2.0 * (15 - i) / 20.0) for i in range(1, 16)])

    def env_db(n, tt):
        """the S-DSP ADSR (dB below full) at times tt after key-on"""
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

    def play_pcm(n, s, ch, T0, T1, gscale=1.0):
        gi = smap[s]
        y8, lp, pk, rt = stored(gi, typ_ratio[s])
        i0 = int(round(T0 / OUT_RATE * PCM_RATE)); i1 = min(total, int(round(T1 / OUT_RATE * PCM_RATE)) + 1)
        if i0 >= total or i1 <= i0: return
        L = len(y8)
        # (positions: per output sample, the step of the pitch at that time - the track, tick by tick)
        tr = n['track']
        ts = np.array([q(e[0]) / OUT_RATE * PCM_RATE for e in tr]) - i0
        idx = np.arange(i1 - i0)
        k = np.clip(np.searchsorted(ts, idx, side='right') - 1, 0, len(tr) - 1)
        step = np.array([e[1] for e in tr])[k] / 4096.0 / rt
        pos = np.floor(np.concatenate([[0.0], np.cumsum(step)[:-1]]))
        if lp is not None and L - lp > 0:
            over = pos >= L
            pos[over] = lp + (pos[over] - lp) % (L - lp)
            ok = np.ones(len(pos), bool)
        else:
            ok = pos < L
        smp = np.where(ok, y8[np.minimum(pos, L - 1).astype(int)], 0.0)
        # volume: 16 levels, one value per tick
        vol = np.array([max(abs(e[2]), abs(e[3])) / 128.0 * e[4] / 128.0 for e in tr])[k]
        amp = G * 2 * 16384.0 * pk / 64.0 * vol * gscale         # (DAC steps for a full-scale stored sample)
        tick_pcm = tick / 32000.0 * PCM_RATE
        tk = np.floor(idx / tick_pcm) * tick_pcm / PCM_RATE
        amp = amp * 10 ** (-env_db(n, tk) / 20.0)
        lvl = np.clip(np.round(15 + 20 * np.log10(np.maximum(amp / 127.0, 1e-6)) / 2.0), 0, 15).astype(int)
        out = np.round(smp * VOL[lvl])           # (the channel's volume table; louder than full scale = full scale)
        mixbuf[ch, i0:] = 0                    # (a new sample on this channel stops the old one)
        mixbuf[ch, i0:i1] = out
        used_pcm[gi] = L
        pcm_end[ch] = T1

    used_pcm = {}
    pcm_ghost = [False] * PCM_CH

    only = int(os.environ.get('FMCONV_VOICES', 'ff'), 16)
    echo_on = os.environ.get('FMCONV_ECHO', '1') != '0'
    # (I24) echo: the S-DSP echo has no Mega Drive counterpart.  FM: a "ghost" of the note - same patch,
    # duller, EVOL quieter, EDL later - on an FM channel nobody needs then (real notes take it back);
    # PCM: the repeats baked into the build-time variant.
    work = []
    for n in notes:
        if not (only >> n['voice'] & 1): continue
        work.append(n)
        if echo_on and n['echo'] and n['edl'] > 0 and n['evol'] > 0.08 and role.get(n['srcn']) in ('fm', 'pcm'):
            g = dict(n); g['ghost'] = n['evol']
            d = int(round(n['edl'] * 32000))
            g['t0'] = n['t0'] + d; g['t1'] = n['t1'] + d
            g['track'] = [(e[0] + d,) + tuple(e[1:]) for e in n['track']]
            work.append(g)
    work.sort(key=lambda n: n['t0'])
    owner = [None] * N_FM            # (the event list of the note on each FM channel, ghost flag)
    fm_ghost = [False] * N_FM
    lists = []

    def cancel(ch, T):
        """a stolen channel: the old note's writes from T on are dropped, a key-off at T"""
        if owner[ch] is None: return
        L = owner[ch]
        L[:] = [e for e in L if e[0] < T]
        part, c = (0, ch) if ch < 3 else (1, ch - 3)
        L.append((T, 0, 0x28, c | (4 if part else 0)))
        owner[ch] = None

    noise_lists = []
    for n in work:
        s = n['srcn']; r = role.get(s, 'skip')
        ghost = n.get('ghost')
        if r == 'skip': stats['skip'] += 1; continue
        T0 = q(n['t0']); T1 = max(q(n['t1']), T0 + int(tick44))
        if T0 >= seconds * OUT_RATE: continue
        t, p, vl, vr, mv = n['track'][0]
        lvl = level_of(n, s, vl, vr, mv) * (ghost or 1.0)
        if lvl <= 0.5: stats['silent'] += 1; continue
        target = r
        if r == 'fm':
            ch = None
            free = [c for c in range(N_FM) if fm_end[c] <= T0]
            if not free and not ghost:
                free = [c for c in range(N_FM) if fm_ghost[c]]          # (real notes take the ghosts' channels)
            if free:
                same = [c for c in free if fm_last[c] == s]
                ch = (same or free)[0]
                if fm_end[ch] > T0: cancel(ch, T0); stats['ghost cut'] += 1
            elif ghost:
                stats['ghost dropped'] += 1; continue
            if ch is None:
                pc = next((c for c in range(PCM_CH) if pcm_end[c] <= T0), None)
                if pc is not None: target = 'pcm'
                else:
                    sc = next((c for c in range(3) if psg_end[c] <= T0), None)
                    if sc is not None: target = 'psg'
                    else:
                        qc = min(range(N_FM), key=lambda c: fm_lvl[c])
                        if fm_lvl[qc] < lvl: ch = qc; cancel(ch, T0); stats['fm steal'] += 1
                        else: stats['dropped'] += 1; continue
        if target == 'noise':
            if ghost: continue
            L = []; lists.append(L)
            inf = info[s]
            ratio = p / 4096.0
            dur = min((T1 - T0) / OUT_RATE, inf['n'] / (32000.0 * ratio))
            rate = 0 if inf['centroid'] * ratio > 7000 else 1
            # (cut the previous hit: one noise channel)
            for LL in noise_lists:
                LL[:] = [e for e in LL if e[0] < T0]
            noise_lists.append(L)
            L.append((T0, 2, 0, 0xE0 | 4 | rate))                  # (white noise, shift rate)
            tk = np.arange(0, dur, tick / 32000.0)
            sdec = inf['decay'] * ratio
            db = env_db(n, tk) + sdec * tk
            last = None
            for i, tt in enumerate(tk):
                a = lvl * 10 ** (-db[i] / 20.0)
                att = int(max(0, min(15, round(20 * math.log10(PSG_RMS / max(a, 1e-3)) / 2.0))))
                if att != last: L.append((T0 + int(tt * OUT_RATE), 2, 0, 0xF0 | att)); last = att
            L.append((T0 + int(dur * OUT_RATE), 2, 0, 0xFF))
            stats['noise'] += 1
            continue
        if target == 'pcm':
            pc = next((c for c in range(PCM_CH) if pcm_end[c] <= T0), None)
            if pc is None and ghost: stats['ghost dropped'] += 1; continue
            if pc is None: pc = next((c for c in range(PCM_CH) if pcm_ghost[c]), None)
            if pc is None: pc = min(range(PCM_CH), key=lambda c: pcm_end[c]); stats['pcm steal'] += 1
            if ghost: T1 = T1 + int(n['edl'] * OUT_RATE * max(0, min(3, abs(n['efb']) * 4)))
            play_pcm(n, s, pc, T0, T1, ghost or 1.0); pcm_ghost[pc] = bool(ghost)
            stats[('pcm ghost' if ghost else 'pcm') + (' (overflow)' if r == 'fm' else '')] += 1
            continue
        inf = info[s]
        L = []; lists.append(L)
        def w(t, part, reg, val, L=L): L.append((int(t), part, reg, val & 0xFF))
        if target == 'psg':
            sc = next(c for c in range(3) if psg_end[c] <= T0)
            psg_end[sc] = T1 + int(tick44); stats['psg'] += 1
            last = None
            for (tt, p, vl, vr, mv) in n['track']:
                T = max(q(tt), T0)
                if T >= T1: break
                freq = inf['f0'] * p / 4096.0
                while freq * 32 * 1023 < PSG_CLK and freq > 0: freq *= 2
                N = max(1, min(1023, int(round(PSG_CLK / (32 * max(freq, 1.0))))))
                a = level_of(n, s, vl, vr, mv)
                att = int(max(0, min(15, round(20 * math.log10(PSG_RMS / max(a, 1e-3)) / 2.0))))
                if (N, att) != last:
                    w(T, 2, 0, 0x80 | sc << 5 | (N & 15)); w(T, 2, 0, N >> 4); w(T, 2, 0, 0x90 | sc << 5 | att); last = (N, att)
            w(T1, 2, 0, 0x90 | sc << 5 | 15)
            continue
        # ---- FM
        stats['fm ghost' if ghost else 'fm'] += 1
        P = patch[s]
        part, c = (0, ch) if ch < 3 else (1, ch - 3)
        kc = c | (4 if part else 0)
        alg = P['alg']; car = fmfit.ALG[alg][1]
        freq0 = inf['f0'] * p / 4096.0
        blk0, _ = fnum_block(freq0)
        ratio = p / 4096.0
        ar, d1r, d1l, d2r = adsr_to_ym(n['adsr1'], n['adsr2'], n['gain'], 0 if inf['looped'] else inf['decay'], ratio)
        # (a slow S-DSP attack is a LINEAR rise; the YM's curve starts too late: AR 31 and the carriers' TL ramped,
        #  the decay held until the ramp ends)
        t_att = 0.0
        if n['adsr1'] & 0x80 and (n['adsr1'] & 15) < 15:
            t_att = 64 * RATE_PERIOD[(n['adsr1'] & 15) * 2 + 1] / 32000.0
        ramp = t_att > 0.03
        kcs = blk0 >> 1
        adj = lambda r_: 0 if r_ == 0 else int(max(1, min(31, r_ - round((kcs - 2) / 2))))
        d1r, d2r = adj(d1r), adj(d2r)
        if not inf['looped']:           # (a non-looped sample ends by itself)
            Tend = T0 + int(inf['n'] / (32000.0 * ratio) * OUT_RATE)
            T1 = min(T1, max(Tend, T0 + int(tick44)))
        if ghost:                       # (the echo fades: EFB repeats ~ a longer release)
            T1 = T1 + int(n['edl'] * OUT_RATE * max(0, min(3, abs(n['efb']) * 4)))
        fm_end[ch] = T1 + int(tick44); fm_last[ch] = s; fm_lvl[ch] = lvl; fm_ghost[ch] = bool(ghost)
        owner[ch] = L
        w(T0, 0, 0x28, kc)                                         # (off first: the new note retriggers)
        w(T0, part, 0xB0 + c, P['fb'] << 3 | alg)
        gscale = ghost or 1.0
        dull = 8 if ghost else 0                                   # (the echo FIR: darker)
        def tl_car(a):
            return 20 * math.log10(max(P['rms'], 1e-6) * FM_FULL / max(a * gscale, 1e-3)) / 0.75
        base = tl_car(lvl / gscale)
        for k in range(4):
            o = SLOT[k] + c
            mul = fmfit.mul_reg(P['mul'][k])
            tl = P['tl'][k] + (base if k in car else dull)
            w(T0, part, 0x30 + o, mul); w(T0, part, 0x40 + o, int(max(0, min(127, round(tl)))))
            if k in car or not inf['looped']:
                w(T0, part, 0x50 + o, 31 if ramp and k in car else ar); w(T0, part, 0x60 + o, 0 if ramp else d1r)
                w(T0, part, 0x70 + o, 0 if ramp else d2r)
                w(T0, part, 0x80 + o, (d1l if k in car else 15 if d1r else 0) << 4 | (9 if ghost else 13))
            else:
                w(T0, part, 0x50 + o, 31); w(T0, part, 0x60 + o, 0); w(T0, part, 0x70 + o, 0); w(T0, part, 0x80 + o, 9 if ghost else 13)
        lastf = lastv = lastpan = None
        Tr = T0 + int(t_att * OUT_RATE) if ramp else T0
        pts = sorted(set([max(q(e[0]), T0) for e in n['track']] +
                         ([T0 + int(i * 0.02 * OUT_RATE) for i in range(int(t_att / 0.02) + 1)] + [Tr] if ramp else [])))
        ti = 0
        for T in pts:
            if T >= T1: break
            while ti + 1 < len(n['track']) and max(q(n['track'][ti + 1][0]), T0) <= T: ti += 1
            (tt, p, vl, vr, mv) = n['track'][ti]
            L_, R_ = abs(vl), abs(vr); m = max(L_, R_, 1)
            pan = (0x80 if L_ > 0.35 * m else 0) | (0x40 if R_ > 0.35 * m else 0)
            if pan != lastpan: w(T, part, 0xB4 + c, pan or 0xC0); lastpan = pan
            freq = inf['f0'] * p / 4096.0
            bf = fnum_block(freq)
            if bf != lastf:
                w(T, part, 0xA4 + c, bf[0] << 3 | bf[1] >> 8); w(T, part, 0xA0 + c, bf[1] & 0xFF); lastf = bf
            a = level_of(n, s, vl, vr, mv)
            b = tl_car(a)
            if ramp and T < Tr:
                b += -20 * math.log10(max((T - T0) / max(Tr - T0, 1), 1e-3)) / 0.75
            if ramp and T == Tr:
                for k in car:
                    o = SLOT[k] + c
                    w(T, part, 0x60 + o, d1r); w(T, part, 0x70 + o, d2r)
            if lastv is None or abs(b - lastv) >= 1 or (ramp and T == Tr):
                if lastv is not None or ramp:
                    for k in car: w(T, part, 0x40 + SLOT[k] + c, int(max(0, min(127, round(P['tl'][k] + b)))))
                lastv = b
        w(T0, 0, 0x28, 0xF0 | kc)
        w(T1, 0, 0x28, kc)
    for L in lists: events.extend(L)
    mix = np.clip(mixbuf.sum(axis=0), -127, 127)
    dac = (mix + 128).astype(np.uint8)
    events.sort(key=lambda e: e[0])
    if report is not None:
        report.update(gain=G, tick=tick, role=role, patch=patch, info=info, stats=dict(stats),
                      variants=len(used_pcm), variant_bytes=int(sum(used_pcm.values())))
    return events, dac

if __name__ == '__main__':
    pack = Pack(sys.argv[1]); secs = float(sys.argv[3])
    cpath = os.environ.get('FMCACHE', '/tmp/claude-0/fm/fitcache.json')
    cache = json.load(open(cpath)) if os.path.exists(cpath) else {}
    rep = {}
    events, dac = convert(pack, sys.argv[2], secs, cache, rep)
    json.dump(cache, open(cpath, 'w'))
    with open(sys.argv[4] + '.ev', 'w') as o:
        for t, p, r, v in events: o.write('%d %d %x %x\n' % (t, p, r, v))
    dac.tofile(sys.argv[4] + '.dac')
    print('gain %.2f tick %.1f (%.1f Hz)' % (rep['gain'], rep['tick'], 32000.0 / rep['tick']), rep['stats'],
          'pcm samples %d (%d KB)' % (rep['variants'], rep['variant_bytes'] // 1024), 'events', len(events))
    for s in sorted(rep['role']):
        i = rep['info'].get(s, {}); p = rep['patch'].get(s)
        print('  srcn %2d %-4s f0 %7.1f %s %s' % (s, rep['role'][s], i.get('f0', 0), 'loop' if i.get('looped') else '    ',
              'alg %d fb %d mul %s tl %s err %.1f' % (p['alg'], p['fb'], p['mul'], p['tl'], p['err']) if p else ''))
