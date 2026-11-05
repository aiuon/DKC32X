#!/usr/bin/env python3
"""(I25) SOUND MD - "Genesis sound only": the 32X plays nothing, the Mega Drive plays everything.

The melody as in SOUND FM (5 FM channels: channel 6 is the DAC), the drums / ambience (the samples SOUND FM
leaves to the 32X) and the sound effects as 8-bit PCM: the 68000 mixes up to 8 voices (one per S-DSP voice:
an effect on voice x takes the music's voice x, as on the SNES) into a ring in the Z80's RAM, the Z80 writes
it to the YM2612's DAC at the timer A rate (TA_UNITS units: 7.6 kHz) and counts the song's ticks in those
same samples - the drums and the FM melody cannot drift apart.

Made here at build time from the user's ROM (nothing of it ships):
  the PCM bank   every sample a drum part or an effect plays, resampled to the DAC rate (its own pitch = a
                 step of 1.0), peak-normalised to 8 bits, padded (the mixer checks a voice's end every
                 CHUNK samples): u16 n, u16 0, then per sample u32 offset (from the bank), u16 length,
                 u16 loop length (0: one-shot); the data
  a song's 8th stream (in its FMS2 block; the 68000 reads it, LZ-packed with the 7 Z80 streams):
     00-3F WAIT n+1   40 lo hi WAIT n   42 END   43 LOOP  (fmpack.py)
     44 v s sh sl l   KEYON voice v: bank sample s, step sh:sl (4.12), level l
     45 v l           LEVEL       46 v sh sl  STEP       47 v  KEYOFF
     level l: 0 = full (the 8-bit sample as stored), each step 1.5 dB less, LEVELS - 1 the quietest
  the effects    per effect (the engine's command number): the same commands on voice 0 (the 68000 moves
                 them to the voice the game names), uncompressed: u16 n, u16 0, u16 offset per effect
                 (0 = none), the streams"""
import math, collections
import numpy as np
import fmconv, fmsong, fmlog

TA_UNITS = 7                    # DAC sample = 7 YM samples: 7610 Hz (NTSC), 7540 (PAL - the FM shifts alike)
YM_RATE = 7670453.0 / 144.0
DAC_RATE = YM_RATE / TA_UNITS
RS_UP, RS_DOWN = 107, 450       # 32 kHz -> the bank's rate (7609 Hz: 0.01 % off the DAC rate)
BANK_RATE = 32000.0 * RS_UP / RS_DOWN
LEVELS = 32
LSTEP = 1.5                     # dB a level
CHUNK = 32                      # (md/fm.c: a voice's end is checked every CHUNK output samples)
PAD = CHUNK * 4 + 8             # (steps < 4.0: 0x3FFF pitch)
K_PCM = 384.0                   # DAC units (+-128 full) per unit of sample at full S-DSP volume (see NOTES I25:
                                # the FM is turned down MD_ATT TL steps to make the room)
MD_ATT = 3                      # (I25) carriers' TL + 3 (2.25 dB) in SOUND MD: the DAC's 8 bits get the headroom
NOISE = 0xFFFF                  # (the S-DSP noise voices: a looped white noise sample in the bank)


def resample(x, up, down):
    """x (input rate) -> up / down of the rate: windowed-sinc lowpass at the new Nyquist (x 0.9), Kaiser window
    (numpy only: the builder's Python has no scipy; values rounded to 1e-9 - the same bytes on every machine)"""
    x = np.asarray(x, float)
    n_out = int(math.ceil(len(x) * up / float(down)))
    if not len(x) or n_out <= 0: return np.zeros(0)
    r = up / float(down)
    fc = 0.5 * min(1.0, r) * 0.9                 # (cycles a sample of x)
    half = int(math.ceil(6.0 / fc))
    t = np.arange(n_out) * (down / float(up))
    k0 = np.floor(t).astype(np.int64)
    y = np.zeros(n_out)
    xp = np.concatenate([np.zeros(half + 1), x, np.zeros(half + 2)])
    for k in range(-half, half + 2):
        d = t - (k0 + k)                          # (distance to tap k0 + k, in samples of x)
        w = np.where(np.abs(d) < half, np.i0(8.0 * np.sqrt(np.maximum(0.0, 1 - (d / half) ** 2))) / np.i0(8.0), 0.0)
        h = 2 * fc * np.sinc(2 * fc * d) * w
        y += xp[k0 + k + half + 1] * np.round(h, 12)
    return np.round(y, 9)


def lin_env(n, dt):
    """the S-DSP envelope (linear, 0..1) of note n at dt seconds after its key-on"""
    if n['adsr1'] & 0x80:
        return 10 ** (-float(fmsong.env_db(n, np.array([dt]))[0]) / 20.0)
    g = n['gain']
    if not g & 0x80: return (g & 0x7F) / 127.0
    mode = (g >> 5) & 3
    rate = g & 31
    if not rate: return 1.0
    per = fmconv.RATE_PERIOD[rate] / 32000.0           # (seconds a step)
    steps = dt / per
    if mode == 0: return max(0.0, 1.0 - steps * 32 / 2048.0)        # linear decrease
    if mode == 1: return max(0.0, (255 / 256.0) ** steps)            # exponential decrease
    return min(1.0, steps * 32 / 2048.0)                            # increases (from 0)


class Bank:
    """the PCM bank: global sample id -> index; the bytes.  order: the samples in their bank order (every
    worker makes the same bank)"""
    def __init__(self, pack, order=()):
        self.pack = pack
        self.idx = {}
        self.ent = []                # (data s8 bytes, length, loop length, peak)
        for gi in order: self.add(gi)

    def add(self, gi):
        if gi in self.idx: return self.idx[gi]
        if gi == NOISE:
            rng = np.random.RandomState(0x5EED)          # (fixed: the same bank everywhere)
            d = rng.randint(-127, 128, size=4096).astype(np.int8)
            data, n, ln, peak = d, 4096, 4096, 0.5
        else:
            pcm, loop = self.pack.sample(gi)
            x = np.asarray(pcm, float)
            n0 = len(x)
            looped = loop < n0 and n0 - loop >= 16
            if looped:
                head = resample(x[:loop], RS_UP, RS_DOWN) if loop > 0 else np.zeros(0)
                lp = x[loop:]
                nl = max(1, int(round(len(lp) * RS_UP / float(RS_DOWN))))
                # (the loop resampled as a period: FFT - its seam stays continuous)
                F = np.fft.rfft(lp)
                keep = min(len(F), nl // 2 + 1)
                G = np.zeros(nl // 2 + 1, complex); G[:keep] = F[:keep]
                if nl % 2 == 0 and keep == nl // 2 + 1: G[-1] = G[-1].real
                body = np.round(np.fft.irfft(G, nl) * (nl / float(len(lp))), 9)
                y = np.concatenate([head, body]); ln = nl
            else:
                y = resample(x, RS_UP, RS_DOWN); ln = 0
            peak = float(np.max(np.abs(y))) if len(y) else 0.0
            peak = max(peak, 1e-4)
            d = np.clip(np.round(y / peak * 127.0), -127, 127).astype(np.int8)
            data, n = d, len(d)
            peak = round(peak, 9)
        # (padding: the loop continued, or silence)
        if ln:
            lpart = data[n - ln:]
            pad = np.tile(lpart, PAD // ln + 2)[:PAD]
        else:
            pad = np.zeros(PAD, np.int8)
        self.idx[gi] = len(self.ent)
        self.ent.append((np.concatenate([data, pad]).astype(np.int8).tobytes(), n, ln, peak))
        return self.idx[gi]

    def blob(self):
        hdr = bytearray(np.array([len(self.ent), 0], '>u2').tobytes())
        o = 4 + 8 * len(self.ent)
        tab = bytearray(); dat = bytearray()
        for data, n, ln, peak in self.ent:
            assert n < 0x10000 and ln < 0x10000
            tab += int(o + len(dat)).to_bytes(4, 'big') + n.to_bytes(2, 'big') + ln.to_bytes(2, 'big')
            dat += data
            if len(dat) & 1: dat += b'\0'
        return bytes(hdr + tab + dat)


def level_of(g):
    """gain (1.0 = the stored sample as is) -> level 0..LEVELS-1, None = silent"""
    if g <= 0: return None
    db = -20 * math.log10(g)
    if db > (LEVELS - 0.5) * LSTEP: return None
    return int(max(0, min(LEVELS - 1, round(db / LSTEP))))


def note_events(n, bank, gi, T0, T1, stats, vslot=None):
    """one S-DSP note -> [(tick, order, bytes)] on voice slot (its own voice unless vslot)"""
    v = n['voice'] if vslot is None else vslot
    if n.get('noise'): gi = NOISE
    bi = bank.add(gi)
    data, nb, ln, peak = bank.ent[bi]
    out = []
    last_l = last_s = None
    ti = 0
    tr = n['track']
    on = False
    pos = 0.0                                  # (bank samples played: a one-shot ends by itself)
    tick_s = fmsong.TICK32 / 32000.0
    for T in range(T0, T1):
        while ti + 1 < len(tr) and max(tr[ti + 1][0], T0) <= T: ti += 1
        (tt, p, vl, vr, mv) = tr[ti]
        if gi == NOISE: p = 4096
        dt = (T - T0) * tick_s
        vf = max(abs(vl), abs(vr)) / 128.0 * mv / 128.0 * lin_env(n, dt)
        g = peak * K_PCM * vf / 127.0
        if g > 1.0: stats['clipped level'] += 1
        l = level_of(g)
        step = int(max(1, min(0x3FFF, p)))
        if not on:
            if l is None:
                if dt > 0.05 and not (n['adsr1'] & 0x80 and lin_env(n, dt + 0.05) > lin_env(n, dt)): break
                pos += step / 4096.0 * DAC_RATE * tick_s
                continue
            out.append((T, 1, bytes([0x44, v, bi, step >> 8, step & 255, l])))
            on = True; last_l, last_s = l, step
        else:
            if l is None:
                out.append((T, 0, bytes([0x47, v]))); on = False; break
            if step != last_s: out.append((T, 2, bytes([0x46, v, step >> 8, step & 255]))); last_s = step
            if l != last_l: out.append((T, 2, bytes([0x45, v, l]))); last_l = l
        pos += step / 4096.0 * DAC_RATE * tick_s
        if not ln and pos >= nb: on = False; break            # (a one-shot over: the mixer stopped it)
    if on: out.append((T1, 0, bytes([0x47, v])))
    stats['pcm notes'] += 1
    return out


def wait_toks(n):
    out = []
    while n > 0:
        if n <= 64: out.append(bytes([n - 1])); n = 0
        else:
            m = min(n, 0xFFFF); out.append(bytes([0x40, m & 255, m >> 8])); n -= m
    return out


def to_tokens(evs, end, loop):
    """[(tick, order, bytes)] -> stream tokens (+ the loop token index), WAITs between"""
    evs = sorted(evs, key=lambda e: (e[0], e[1]))
    out, t, i, lt = [], 0, 0, None
    while True:
        if loop is not None and lt is None and t >= loop: lt = len(out)
        while i < len(evs) and evs[i][0] <= t:
            out.append(evs[i][2]); i += 1
        if t >= end: break
        nxt = evs[i][0] if i < len(evs) else end
        if loop is not None and lt is None and t < loop < nxt: nxt = loop
        nxt = min(max(nxt, t + 1), end)
        out += wait_toks(nxt - t)
        t = nxt
    out.append(bytes([0x43 if loop is not None else 0x42]))
    return out, lt


def drum_stream(S, notes, smap, bank, stats):
    """the song's PCM part (the S-DSP voices of its 'spc' samples): stream tokens, loop token index"""
    evs = []
    for si, (a, b) in enumerate(S.segs):
        for n in notes:
            k0 = n['t0']
            if not (a <= k0 < b): continue
            if S.role.get(n['srcn']) != 'spc' and not n.get('noise'): continue
            gi = smap.get(n['srcn'])
            if gi is None and not n.get('noise'): continue
            T1 = min(max(n['t1'], k0 + 1), b)
            evs += note_events(n, bank, gi, k0, T1, stats)
        # (whatever sounds at the segment's end is cut: the loop starts clean)
        for v in range(8): evs.append((b, -1, bytes([0x47, v])))
    # (keyoffs that end nothing are harmless but cost bytes: drop those of voices not sounding)
    evs.sort(key=lambda e: (e[0], e[1]))
    live = [False] * 8; keep = []
    for e in evs:
        op, v = e[2][0], e[2][1]
        if op == 0x44: live[v] = True
        elif op == 0x47:
            if not live[v]: continue
            live[v] = False
        keep.append(e)
    return to_tokens(keep, S.end, S.loop)


SFX_TICKS = 400                 # (5 s: the longest effect)


def sfx_notes(gen, le, song, cmd, music=False):
    """the S-DSP notes effect cmd plays on voice 0 with this song's ARAM -> (notes, smap) or None"""
    aram, smp = fmlog.song_aram(le, song)
    s = fmlog.SPC32(bytearray(aram), bytes(128), gen)
    s.stack = ['__end__']; s.run('CODE_1071', '__end__')
    s.m[0x24] = 1 if music else 0
    s.m[0x4D8] = 0; s.a = cmd; s.x = 0
    s.log = []
    s.stack = ['__end__']
    try:
        s.run('CODE_1173', '__end__')
    except Exception:
        return None
    ev = [(0, r, v) for r, v in s.log]
    if not s.m[0x1A0]: return None
    for k in range(1, SFX_TICKS):
        s.log = []; s.stack = ['__end__']
        try:
            s.run('CODE_064F', 'CODE_05FE')
        except Exception:
            break
        ev += [(k, r, v) for r, v in s.log]
        if not s.m[0x1A0]:
            ev.append((k + 1, 0x5C, 1))           # (its flag cleared: the voice released)
            break
    smap = {i: g for i, g in enumerate(smp) if g != 0xFFFF}
    notes = [n for n in fmconv.notes_of(ev) if n['voice'] == 0]
    other = [n for n in fmconv.notes_of(ev) if n['voice'] != 0]
    return notes, smap, other


def sfx_stream(notes, smap, bank, stats):
    """an effect's notes -> its stream bytes.  The song's master volume is left out (full): the 68000 adds the
    playing song's (sfx level offset, fm.json); the engine's first ticks before the key-on are left out (the
    PCM ring's own delay instead)"""
    evs = []
    end = 0
    if not notes: return None
    t00 = min(n['t0'] for n in notes)
    notes = [dict(n, t0=n['t0'] - t00, t1=n['t1'] - t00,
                  track=[(max(0, e[0] - t00),) + tuple(e[1:4]) + (127,) for e in n['track']]) for n in notes]
    for n in notes:
        gi = smap.get(n['srcn'])
        if gi is None and not n.get('noise'): continue
        T1 = max(n['t1'], n['t0'] + 1)
        evs += note_events(n, bank, gi, n['t0'], T1, stats, vslot=0)
        end = max(end, T1)
    if not evs: return None
    toks, _ = to_tokens(evs, end, None)
    return b''.join(toks)


def sfx_table(streams):
    """{cmd: bytes} -> the effects table blob"""
    n = max(streams) + 1 if streams else 0
    hdr = bytearray(n.to_bytes(2, 'big') + b'\0\0')
    o = 4 + 2 * n
    body = bytearray(); offs = []
    seen = {}
    for c in range(n):
        st = streams.get(c)
        if st is None: offs.append(0); continue
        if st in seen: offs.append(seen[st]); continue
        seen[st] = o + len(body); offs.append(o + len(body))
        body += st
    assert o + len(body) < 0x10000
    for x in offs: hdr += x.to_bytes(2, 'big')
    return bytes(hdr + body)


def song_mv(notes):
    """the song's master volume (its first note's)"""
    for n in notes:
        if n['track']: return max(1, n['track'][0][4])
    return 127


def sfx_att(mv):
    """the levels an effect plays quieter in a song of master volume mv (the S-DSP's MVOL scales the effects too)"""
    return int(max(0, min(LEVELS - 1, round(-20 * math.log10(mv / 127.0) / LSTEP))))


def all_sfx(gen, le, song, bank_add=None):
    """every effect with this song's ARAM -> {cmd: (notes, smap)}"""
    out = {}
    for cmd in range(1, 0xFC):
        r = sfx_notes(gen, le, song, cmd)
        if r is None or not r[0]: continue
        out[cmd] = (r[0], r[1])
    return out
