#!/usr/bin/env python3
"""Song extraction from an ARAM snapshot: run the translated engine from its init, collect DSP
writes with 32 kHz timestamps, decode the samples it uses.

usage: song.py aram.bin seconds outprefix    -> outprefix.ev / outprefix.smp (for render_test)
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import spcrun  # noqa: E402
from brr import decode_brr, prefilter  # noqa: E402


def start_song(gen, aram, dsp=None):
    s = spcrun.SPC(aram, dsp or bytes(128), gen)
    s.stack = ['__done__']
    s.B['__done__'] = None
    pc = 'CODE_1071'
    while pc != '__done__':
        pc = s.B[pc]()
    s.m[0x24] = 1
    return s


def events(s, seconds):
    """-> list of (t32, reg, val); init writes at t=0"""
    ev = [(0, r, v) for r, v in s.log]
    s.log = []
    t = 0
    while t < seconds * 32000:
        t += 4 * (s.m[0xEC] or 256)
        s.tick()
        ev += [(t, r, v) for r, v in s.log]
        s.log = []
    return ev


def samples(aram, dir_page, srcns):
    out = {}
    for n in srcns:
        e = dir_page * 256 + n * 4
        start = aram[e] | aram[e + 1] << 8
        loop = aram[e + 2] | aram[e + 3] << 8
        pcm16, li = decode_brr(aram, start, loop)
        out[n] = (pcm16, li)
    return out


if __name__ == '__main__':
    aram = open(sys.argv[1], 'rb').read()
    secs = float(sys.argv[2])
    pre = sys.argv[3]
    gen = spcrun.load_engine_module()
    s = start_song(gen, aram)
    ev = events(s, secs)
    used = sorted(set(v for t, r, v in ev if r & 0x0F == 4))
    dirp = [v for t, r, v in ev if r == 0x5D]
    dirp = dirp[-1] if dirp else 0x32
    smp = samples(aram, dirp, used)
    with open(pre + '.ev', 'wb') as f:
        for t, r, v in ev:
            f.write(struct.pack('<IBB', t, r, v))
    with open(pre + '.smp', 'wb') as f:
        f.write(struct.pack('<I', 256))
        for n in range(256):
            if n in smp:
                pcm16, li = smp[n]
                pcm = prefilter(pcm16, li)
                loop = li if li is not None else len(pcm)
                pad = pcm[li] if li is not None else 0
                f.write(struct.pack('<IIII', 1, len(pcm), loop, 0))
                f.write(struct.pack('<%dh' % (len(pcm) + 1), *(pcm + [pad])))
            else:
                f.write(struct.pack('<IIII', 0, 0, 0, 0))
    print(len(ev), 'events; samples', used, 'dir %02X' % dirp)
