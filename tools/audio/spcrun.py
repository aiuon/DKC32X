#!/usr/bin/env python3
"""Run the translated DKC sound engine (spc2x.py, Python backend) from an ARAM snapshot and
produce its DSP register writes per timer tick."""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..'))
import paths
import importlib.util
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ASM = os.path.join(paths.DISASM, 'SPC700/SPC700_Engine_DKC1.asm')


def load_engine_module(cache=os.path.join(paths.TMP, 'dkc_engine_gen.py')):
    os.makedirs(os.path.dirname(cache), exist_ok=True)
    subprocess.check_call([sys.executable, os.path.join(HERE, 'spc2x.py'), ASM, 'py', cache],
                          stdout=subprocess.DEVNULL)
    spec = importlib.util.spec_from_file_location('dkc_engine_gen', cache)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class SPC:
    def __init__(self, aram, dspregs, gen):
        self.m = bytearray(aram)
        self.dsp = bytearray(dspregs)
        self.a = self.x = self.y = 0
        self.n = self.z = self.c = 0
        self.stack = []
        self.log = []          # (addr, value) DSP writes
        self.ports_in = [0, 0, 0, 0]
        self.B = gen.build(self)
        self.reads = set()     # ARAM addresses read (song data tracing)

    # ---- memory
    def rd(self, a):
        if 0xF0 <= a <= 0xFF:
            if a == 0xF3:
                return self.dsp[self.m[0xF2] & 0x7F]
            if 0xF4 <= a <= 0xF7:
                return self.ports_in[a - 0xF4]
            if a in (0xFD, 0xFE, 0xFF):
                return 1
            return self.m[a]
        self.reads.add(a)
        return self.m[a]

    def wr(self, a, v):
        v &= 0xFF
        if a == 0xF3:
            r = self.m[0xF2]
            if r < 0x80:
                self.dsp[r] = v
                self.log.append((r, v))
            return
        self.m[a] = v

    def rw(self, a):
        return self.rd(a) | self.rd((a + 1) & 0xFFFF) << 8

    # ---- flags/alu
    def nz(self, v):
        v &= 0xFF
        self.n = v >> 7
        self.z = int(v == 0)
        return v

    def nz16(self, v):
        self.n = (v >> 15) & 1
        self.z = int((v & 0xFFFF) == 0)

    def adc(self, a, b):
        r = a + b + self.c
        self.c = int(r > 0xFF)
        return self.nz(r)

    def sbc(self, a, b):
        r = a - b - (1 - self.c)
        self.c = int(r >= 0)
        return self.nz(r)

    def cmp(self, a, b):
        r = a - b
        self.c = int(r >= 0)
        self.nz(r)

    def asl(self, v):
        self.c = v >> 7
        return self.nz(v << 1)

    def lsr(self, v):
        self.c = v & 1
        return self.nz(v >> 1)

    def ror(self, v):
        c = self.c
        self.c = v & 1
        return self.nz(v >> 1 | c << 7)

    def ya(self):
        return self.y << 8 | self.a

    def setya(self, w):
        w &= 0xFFFF
        self.a, self.y = w & 0xFF, w >> 8

    def addw(self, d):
        w = self.ya() + self.rw(d)
        self.c = int(w > 0xFFFF)
        self.setya(w)
        self.nz16(w)

    def subw(self, d):
        w = self.ya() - self.rw(d)
        self.c = int(w >= 0)
        self.setya(w)
        self.nz16(w)

    def cmpw(self, d):
        w = self.ya() - self.rw(d)
        self.c = int(w >= 0)
        self.nz16(w)

    # ---- driving
    def run(self, label, stop, limit=200000):
        pc = label
        n = 0
        while pc != stop:
            pc = self.B[pc]()
            n += 1
            if n > limit:
                raise RuntimeError('runaway at %s' % pc)
        return n

    def tick(self):
        """one timer-0 period: body from CODE_064F back to the port poll (CODE_05FE)"""
        self.stack = []
        return self.run('CODE_064F', 'CODE_05FE')


if __name__ == '__main__':
    gen = load_engine_module()
    aram = open(sys.argv[1], 'rb').read()
    dsp = open(sys.argv[2], 'rb').read()
    s = SPC(aram, dsp, gen)
    for t in range(int(sys.argv[3]) if len(sys.argv) > 3 else 100):
        s.tick()
    print(len(s.log), 'writes')
