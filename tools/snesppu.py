#!/usr/bin/env python3
"""A small SNES PPU background renderer (modes 1, 5, 7; no sprites, no colour math): VRAM + CGRAM +
registers -> a 256x224 picture of CGRAM indices.  The front-end screens (title, file select...)
are checked against snes9x with it; the SH-2 renderer (sh/fescr.c) does the same live."""
import struct
import numpy as np

BPP = {0: (2, 2, 2, 2), 1: (4, 4, 2, 0), 3: (8, 4, 0, 0), 5: (4, 2, 0, 0), 7: (8, 0, 0, 0)}


def planar_tile(v, addr, bpp):
    """8x8 tile at byte address addr -> 64 pixel values"""
    out = np.zeros(64, np.int32)
    for row in range(8):
        for pl in range(bpp // 2):
            lo = v[(addr + pl * 16 + row * 2) & 0xFFFF]
            hi = v[(addr + pl * 16 + row * 2 + 1) & 0xFFFF]
            for col in range(8):
                sh = 7 - col
                out[row * 8 + col] |= ((lo >> sh) & 1) << (pl * 2) | ((hi >> sh) & 1) << (pl * 2 + 1)
    return out


class Regs:
    """regs: the harness PPU register dump (0x1001: written values by address) + bg.bin (0x1003)"""
    def __init__(self, regs, bg):
        self.r = regs
        self.bg = [struct.unpack_from('<HHHBBHH', bg, i * 12) for i in range(4)]

    def __getitem__(self, a):
        return self.r[a]


def render(vram, cgram, regs, layers=None, hires_odd=False):
    """-> (idx[224,256] CGRAM index, opaque[224,256])"""
    v = vram
    mode = regs[0x2105] & 7
    tm = regs[0x212C] if layers is None else layers
    W, H = 256, 224
    idx = np.zeros((H, W), np.int32)
    if mode == 7:
        if tm & 1:
            return m7(v, regs), None
        return idx, None
    bpps = BPP.get(mode, (4, 4, 2, 0))
    cache = {}
    # back to front: (layer, priority)
    bg3hi = (regs[0x2105] & 8) and mode == 1
    order = [(2, 0), (2, 1)] if not bg3hi else [(2, 0)]
    order += [(1, 0), (0, 0), (1, 1), (0, 1)]
    if bg3hi:
        order += [(2, 1)]
    for li, pr in order:
        if not (tm >> li) & 1 or li >= len(bpps) or not bpps[li]:
            continue
        bpp = bpps[li]
        sc, h, vv, size, _, name, scs = regs.bg[li]
        # sc = map word address, name = char word address (harness layout); scs = map size bits
        hires = mode in (5, 6)
        for y in range(H):
            my = (y + vv) & 0x3FF
            for x in range(W):
                hx = x * 2 + (1 if hires_odd else 0) if hires else x
                mx = (hx + h) & 0x3FF
                tw = 16 if hires else 8
                tx, ty = (mx // tw), my >> 3
                a = sc + (ty & 31) * 32 + (tx & 31)
                if (scs & 1) and (tx & 32): a += 0x400
                if (scs & 2) and (ty & 32): a += 0x400 * (2 if scs & 1 else 1)
                e = v[(a * 2) & 0xFFFF] | v[(a * 2 + 1) & 0xFFFF] << 8
                if ((e >> 13) & 1) != pr:
                    continue
                t = e & 0x3FF
                px, py = (mx % tw), my & 7
                if e & 0x4000: px = tw - 1 - px
                if e & 0x8000: py = 7 - py
                if hires:
                    t += px >> 3
                    px &= 7
                key = (name, t, bpp)
                if key not in cache:
                    cache[key] = planar_tile(v, (name * 2 + t * bpp * 8) & 0xFFFF, bpp)
                c = cache[key][py * 8 + px]
                if c:
                    p = (e >> 10) & 7
                    idx[y, x] = c if bpp == 8 else p * (1 << bpp) + c
    return idx, None


def m7(v, regs, mat=None):
    """mode 7 with the matrix (a, b, c, d, cx, cy, h, v) - from regs if not given"""
    W, H = 256, 224
    idx = np.zeros((H, W), np.int32)
    if mat is None:
        return idx
    a, b, c, d, cx, cy, hs, vs = mat
    for y in range(H):
        for x in range(W):
            X = (a * (x + hs - cx) + b * (y + vs - cy) >> 8) + cx
            Y = (c * (x + hs - cx) + d * (y + vs - cy) >> 8) + cy
            if X < 0 or Y < 0 or X >= 1024 or Y >= 1024:
                continue
            t = v[((Y >> 3) * 128 + (X >> 3)) * 2]
            idx[y, x] = v[(t * 64 + (Y & 7) * 8 + (X & 7)) * 2 + 1]
    return idx


def to_rgb(idx, cgram, bright=15):
    cg = np.frombuffer(cgram, '<u2').astype(np.int32)
    c = cg[idx]
    rgb = np.stack([(c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3], -1)
    return (rgb * (bright + 1) // 16).astype(np.uint8)
