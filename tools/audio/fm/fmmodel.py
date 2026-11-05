#!/usr/bin/env python3
"""(I24) SOUND FM: the Z80 driver's behaviour in Python (z80/fmdrv.asm does the same): a song's commands ->
YM2612 / PSG register writes (time in DAC samples) + the DAC's 8-bit stream.  Used for previews and as the
reference the Z80 driver is checked against (devkit/fm harness)."""
import math
import numpy as np
from fmsong import TICK_LEN, PCM_RATE, N_FM, N_PCM, SLOT


class Model:
    def __init__(self, song, mono=False, att=0):
        self.s = song
        self.att = att                   # (MB_ATT: added to the carriers' TL - I25 SOUND MD)
        self.mono = mono
        self.writes = []                 # (dac sample, part 0/1/2=psg, reg, val)
        self.inst = [None] * N_FM
        self.env_ = [None] * N_FM
        self.vol = [0] * N_FM
        self.pan = [3] * N_FM

    def w(self, t, part, reg, val): self.writes.append((t, part, reg, val & 0xFF))

    def init(self, t):
        w = self.w
        w(t, 0, 0x22, 0); w(t, 0, 0x27, 0); w(t, 0, 0x2B, 0x80); w(t, 1, 0xB6, 0xC0)
        for c in range(N_FM):
            part, cc = (0, c) if c < 3 else (1, c - 3)
            w(t, 0, 0x28, cc | part << 2)
        for b in (0x9F, 0xBF, 0xDF, 0xFF): w(t, 2, 0, b)

    def ch(self, c): return (0, c) if c < 3 else (1, c - 3)

    def run(self, nticks_max, loops=1):
        """-> DAC u8 stream; the song plays to its end (or through `loops` passes of its body)"""
        s = self.s
        cmds = s.cmds
        self.init(0)
        n_total = int(math.ceil(min(nticks_max, s.end if s.loop is None else s.loop + (s.end - s.loop) * loops) * TICK_LEN)) + 1
        n_total = int(min(n_total, nticks_max * TICK_LEN)) + 1
        dac = np.zeros(n_total, np.int32)
        pcm_play = [None] * N_PCM        # (variant, start sample)
        pcm_spans = []
        # the stream as the Z80 meets it: intro, then the body again and again
        off = 0.0                        # (tick -> sample: the body's later passes are shifted)
        passes = [(0, s.end, 0)]
        if s.loop is not None:
            k = s.end
            while k < nticks_max:
                passes.append((s.loop, s.end, k - s.loop)); k += s.end - s.loop
        for a, b, shift in passes:
            for (k, seq, c) in cmds:
                if not (a <= k < b) and not (k == b and c[0] in ('keyoff', 'pcmstop', 'psg')): continue
                if shift and k < a: continue
                kk = k + shift
                if kk >= nticks_max: break
                t = int(kk * TICK_LEN)
                self.do(t, c, pcm_play, pcm_spans)
            # (the jump: everything still sounding is cut)
            t = int(min(b + shift, nticks_max) * TICK_LEN)
            for p in range(N_PCM):
                if pcm_play[p]: pcm_spans.append((pcm_play[p][0], pcm_play[p][1], t)); pcm_play[p] = None
        for p in range(N_PCM):
            if pcm_play[p]: pcm_spans.append((pcm_play[p][0], pcm_play[p][1], n_total))
        for vi, t0, t1 in pcm_spans:
            y8, lp = s.var[vi]
            m = max(0, min(t1, n_total) - t0)
            if m <= 0: continue
            if lp is not None:
                body = y8[lp:] if len(y8) > lp else y8[-1:]
                seg = np.concatenate([y8[:lp], np.tile(body, (m - lp) // max(len(body), 1) + 2)])[:m]
            else:
                seg = y8[:m]
            dac[t0:t0 + len(seg)] += seg.astype(np.int32)
        self.writes.sort(key=lambda e: e[0])
        # (the Z80 adds without saturation: wrap like it does)
        return ((dac + 128) & 0xFF).astype(np.uint8)

    def do(self, t, c, pcm_play, pcm_spans):
        w = self.w
        op = c[0]
        if op == 'keyon':
            _, ch, iid, eid, fn, vol, pan = c
            part, cc = self.ch(ch)
            b0, cm, follow, ops = self.s.inst[iid]
            e50, e60, e70, e80, p60, p70, dull = self.s.env[eid]
            w(t, 0, 0x28, cc | part << 2)
            w(t, part, 0xB0 + cc, b0)
            for k in range(4):
                o = SLOT[k] + cc
                mul, tl = ops[k]
                car = cm >> k & 1
                w(t, part, 0x30 + o, mul)
                w(t, part, 0x40 + o, min(127, tl + vol + self.att) if car else min(127, tl + dull))
                if car:
                    w(t, part, 0x50 + o, e50); w(t, part, 0x60 + o, e60); w(t, part, 0x70 + o, e70); w(t, part, 0x80 + o, e80)
                elif follow:            # (the modulators fade with the note: its decay, to silence)
                    w(t, part, 0x50 + o, e50); w(t, part, 0x60 + o, p60); w(t, part, 0x70 + o, p70)
                    w(t, part, 0x80 + o, (0xF0 if p60 else 0) | (e80 & 15))
                else:
                    w(t, part, 0x50 + o, 31); w(t, part, 0x60 + o, 0); w(t, part, 0x70 + o, 0); w(t, part, 0x80 + o, e80 & 15)
            self.inst[ch], self.env_[ch], self.vol[ch], self.pan[ch] = iid, eid, vol, pan
            w(t, part, 0xB4 + cc, 0xC0 if self.mono else pan << 6)
            w(t, part, 0xA4 + cc, fn >> 8); w(t, part, 0xA0 + cc, fn & 0xFF)
            w(t, 0, 0x28, 0xF0 | cc | part << 2)
        elif op == 'freq':
            _, ch, fn = c
            part, cc = self.ch(ch)
            w(t, part, 0xA4 + cc, fn >> 8); w(t, part, 0xA0 + cc, fn & 0xFF)
        elif op == 'vol':
            _, ch, vol = c
            part, cc = self.ch(ch)
            if self.inst[ch] is None: return
            b0, cm, follow, ops = self.s.inst[self.inst[ch]]
            self.vol[ch] = vol
            for k in range(4):
                if cm >> k & 1: w(t, part, 0x40 + SLOT[k] + cc, min(127, ops[k][1] + vol + self.att))
        elif op == 'env2':
            _, ch = c
            part, cc = self.ch(ch)
            if self.inst[ch] is None: return
            b0, cm, follow, ops = self.s.inst[self.inst[ch]]
            e = self.s.env[self.env_[ch]]
            for k in range(4):
                if cm >> k & 1:
                    w(t, part, 0x60 + SLOT[k] + cc, e[4]); w(t, part, 0x70 + SLOT[k] + cc, e[5])
        elif op == 'pan':
            _, ch, pan = c
            part, cc = self.ch(ch)
            self.pan[ch] = pan
            w(t, part, 0xB4 + cc, 0xC0 if self.mono else pan << 6)
        elif op == 'keyoff':
            _, ch = c
            part, cc = self.ch(ch)
            w(t, 0, 0x28, cc | part << 2)
        elif op == 'psg':
            for b in c[1]: w(t, 2, 0, b)
        elif op == 'pcm':
            _, p, vi = c
            if pcm_play[p]: pcm_spans.append((pcm_play[p][0], pcm_play[p][1], t))
            pcm_play[p] = (vi, t)
        elif op == 'pcmstop':
            _, p = c
            if pcm_play[p]: pcm_spans.append((pcm_play[p][0], pcm_play[p][1], t)); pcm_play[p] = None


def preview(song, seconds, base, mono=False):
    """events for the preview renderer (44.1 kHz) + the DAC stream (PCM_RATE)"""
    m = Model(song, mono)
    nt = seconds * PCM_RATE / TICK_LEN
    dac = m.run(nt, loops=99)
    with open(base + '.ev', 'w') as o:
        for t, p, r, v in m.writes:
            o.write('%d %d %x %x\n' % (int(t / PCM_RATE * 44100), p, r, v))
    dac.tofile(base + '.dac')
    return m
