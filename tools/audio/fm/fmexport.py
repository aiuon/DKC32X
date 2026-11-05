#!/usr/bin/env python3
"""(I24) SOUND FM: song data for the Z80 driver - the tables as the Z80 keeps them, and (devkit) a feed file
for devkit/fm/zharness (the 68000's part unrolled: streams expanded, loops repeated)."""
import struct
import fmpack
from fmsong import TICK32

YM_NTSC, YM_PAL = 7670453.0, 7600489.0


def tick_units(pal=False):
    """a tick (12.5 ms) in YM timer A units x 256 (timer A unit = 144 YM clocks)"""
    clk = YM_PAL if pal else YM_NTSC
    return int(round(TICK32 / 32000.0 * clk / 144.0 * 256))


def inst_bytes(song):
    out = bytearray()
    for b0, cm, follow, ops in song.inst:
        out += bytes([b0, cm, follow] + [x for op in ops for x in op])
    return bytes(out)


def env_bytes(song):
    return b''.join(bytes(e) for e in song.env)


def feed(song, path, loops=3, flags=0, delay=0):
    streams, lts = fmpack.tokens(song)
    with open(path, 'wb') as f:
        ib, eb = inst_bytes(song), env_bytes(song)
        f.write(struct.pack('<4sIIIII', b'FMF1', tick_units(flags & 1), flags, delay, len(song.inst), len(song.env)))
        f.write(ib); f.write(eb)
        for toks, lt in zip(streams, lts):
            body = b''.join(toks[:-1])
            if toks[-1][0] == 0x43 and lt is not None:
                data = b''.join(toks[:lt]) + b''.join(toks[lt:-1]) * loops + bytes([0x42])
            else:
                data = body + bytes([0x42])
            f.write(struct.pack('<I', len(data))); f.write(data)
