#!/usr/bin/env python3
"""(I25) SOUND MD: the 68000's PCM mixer in Python (md/fm.c does the same, to the byte) - previews and the
reference the 68000 code is checked against.

Voices 0-7 (an S-DSP voice each); a voice: bank sample, position (offset + 16-bit fraction), step (4.12 from the
stream, << 4 = 16.16), level.  Mixing: acc[i] += VOL[level][byte] for every voice on; the voice's end is
checked after each piece (<= CHUNK samples: the bank's padding covers the overrun); the DAC byte =
clip(acc, -128, 127) + 128.  Ticks: the Z80's rule (ACC += 256 x TA_UNITS a DAC sample, a tick when >= TK)."""
import struct
import numpy as np
import mdconv

CHUNK = mdconv.CHUNK
AINC = 256 * mdconv.TA_UNITS


def vol_table():
    """VOL[level][byte] (signed bytes; the byte's two's complement as the index)"""
    t = np.zeros((mdconv.LEVELS, 256), np.int8)
    for l in range(mdconv.LEVELS):
        # (integer: gain = 2^-(l x 1.5 / 6.0206) in 1/256: the 68000 builds the same table with muls)
        g = int(round(256 * 10 ** (-l * mdconv.LSTEP / 20.0)))
        for b in range(256):
            s = b - 256 if b >= 128 else b
            t[l, b] = (s * g) >> 8                   # (muls + asr: rounds down)
    return t


def gains():
    return [int(round(256 * 10 ** (-l * mdconv.LSTEP / 20.0))) for l in range(mdconv.LEVELS)]


class Bank:
    def __init__(self, blob):
        self.b = blob
        n = struct.unpack_from('>H', blob, 0)[0]
        self.ent = [struct.unpack_from('>IHH', blob, 4 + 8 * i) for i in range(n)]


class Voice:
    __slots__ = ('on', 'smp', 'pos', 'frac', 'step', 'lvl', 'n', 'ln', 'sfx')

    def __init__(self):
        self.on = 0; self.smp = 0; self.pos = 0; self.frac = 0; self.step = 0; self.lvl = 0; self.n = 0; self.ln = 0
        self.sfx = None


class Stream:
    """a command stream (already expanded: a list of commands) with the Z80's WAIT rule"""
    def __init__(self, cmds, loop_index=None):
        self.c = cmds; self.i = 0; self.wait = 0; self.loop = loop_index; self.done = False


class Mixer:
    def __init__(self, bank_blob, tk, sfx_blob=None):
        self.bank = Bank(bank_blob)
        self.vt = vol_table()
        self.v = [Voice() for _ in range(8)]
        self.tk = tk
        self.music = None; self.m_next = None; self.m_acc = 0
        self.sfx_next = 0; self.sfx_acc = 0
        self.sfx = sfx_blob
        self.sfx_att = 0
        self.t = 0                                   # samples made

    # ---- commands
    def cmd(self, c, slot_of=None, sfx=None):
        op = c[0]
        if op == 0x44:
            v = self.v[c[1] if slot_of is None else slot_of]
            if sfx is None and v.sfx is not None: return
            o, n, ln = self.bank.ent[c[2]]
            v.on = 1; v.smp = o; v.pos = 0; v.frac = 0; v.step = (c[3] << 8 | c[4]) << 4
            v.lvl = min(mdconv.LEVELS - 1, c[5] + (self.sfx_att if sfx is not None else 0)); v.n = n; v.ln = ln
        elif op == 0x45:
            v = self.v[c[1] if slot_of is None else slot_of]
            if sfx is None and v.sfx is not None: return
            v.lvl = min(mdconv.LEVELS - 1, c[2] + (self.sfx_att if sfx is not None else 0))
        elif op == 0x46:
            v = self.v[c[1] if slot_of is None else slot_of]
            if sfx is None and v.sfx is not None: return
            v.step = (c[2] << 8 | c[3]) << 4
        elif op == 0x47:
            v = self.v[c[1] if slot_of is None else slot_of]
            if sfx is None and v.sfx is not None: return
            v.on = 0

    def run_stream(self, st, slot_of=None, sfx=None):
        """a tick of stream st: its commands up to the next WAIT"""
        if st.done: return
        if st.wait:
            st.wait -= 1
            if st.wait: return
        while True:
            if st.i >= len(st.c):
                st.done = True; return
            c = st.c[st.i]; st.i += 1
            op = c[0]
            if op < 0x40: st.wait = op + 1; return
            if op == 0x40: st.wait = c[1] | c[2] << 8; return
            if op == 0x42: st.done = True; return
            if op == 0x43:
                if st.loop is None: st.done = True; return
                st.i = st.loop; continue
            self.cmd(c, slot_of, sfx)

    def start_song(self, cmds, loop_index, at):
        """the song's tick 0 before sample `at`"""
        self.music = Stream(cmds, loop_index)
        self.m_next = at; self.m_acc = 0

    def play_sfx(self, stream_cmds, slot):
        v = self.v[slot]
        v.sfx = Stream(stream_cmds); v.on = 0

    # ---- mixing
    def mix(self, n):
        out = np.zeros(n, np.uint8)
        done = 0
        while done < n:
            # (ticks due now: the song's, the effects')
            while self.music is not None and self.m_next is not None and self.t >= self.m_next:
                self.run_stream(self.music)
                j = -(-(self.tk - self.m_acc) // AINC)
                self.m_acc += j * AINC - self.tk
                self.m_next += j
            while self.t >= self.sfx_next:
                for k, v in enumerate(self.v):
                    if v.sfx is not None:
                        self.run_stream(v.sfx, slot_of=k, sfx=True)
                        if v.sfx.done: v.sfx = None
                j = -(-(self.tk - self.sfx_acc) // AINC)
                self.sfx_acc += j * AINC - self.tk
                self.sfx_next += j
            m = min(n - done, CHUNK, self.sfx_next - self.t)
            if self.music is not None and self.m_next is not None: m = min(m, self.m_next - self.t)
            m = max(1, m)
            acc = np.zeros(m, np.int32)
            b = self.bank.b
            for v in self.v:
                if not v.on: continue
                tab = self.vt[v.lvl]
                pos, frac, st = v.pos, v.frac, v.step
                for i in range(m):
                    acc[i] += tab[b[v.smp + pos]]
                    frac += st & 0xFFFF
                    pos += (st >> 16) + (frac >> 16)
                    frac &= 0xFFFF
                v.pos, v.frac = pos, frac
                if pos >= v.n:
                    if v.ln:
                        while v.pos >= v.n: v.pos -= v.ln
                    else:
                        v.on = 0
            out[done:done + m] = (np.clip(acc, -128, 127) + 128).astype(np.uint8)
            done += m
            self.t += m
        return out


def expand_stream(blob, start, loop_off):
    """an 8th-stream (packed) -> (commands, loop command index)"""
    import fmpack
    cmds = fmpack.expand(blob, start)
    li = None
    if loop_off is not None:
        # (the loop offset is into the blob: find the command index the expansion reaches it at)
        li = _loop_index(blob, start, loop_off)
    return cmds, li


def _loop_index(blob, start, loop_off):
    import fmpack
    stack = []; pos, end = start, len(blob); k = 0
    while True:
        if stack and pos >= end:
            pos, end = stack.pop(); continue
        if pos == loop_off and not stack: return k
        op = blob[pos]
        L = fmpack.cmd_len(blob, pos)
        if op == 0x41 or 0x60 <= op < 0x80:
            stack.append((pos + L, end))
            o = blob[pos + 1] | blob[pos + 2] << 8
            pos, end = o, o + (blob[pos + 3] if op == 0x41 else op - 0x5D)
            continue
        k += 1; pos += L
        if op in (0x42, 0x43): return None


def sfx_cmds(sfx_blob, cmd):
    """an effect's commands (uncompressed stream) or None"""
    import fmpack
    n = struct.unpack_from('>H', sfx_blob, 0)[0]
    if cmd >= n: return None
    o = struct.unpack_from('>H', sfx_blob, 4 + 2 * cmd)[0]
    if not o: return None
    out = []
    while True:
        L = fmpack.cmd_len(sfx_blob, o)
        out.append(bytes(sfx_blob[o:o + L])); op = sfx_blob[o]; o += L
        if op in (0x42, 0x43): return out
