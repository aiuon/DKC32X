"""Research: check my layout model against SNES BG1 tilemaps captured at level start."""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '.'))
import paths
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dkcrom import Rom

S = paths.SURVEY


def level_params(e):
    r = open('%s/%s/ram.bin' % (S, e), 'rb').read()
    w = lambda a: r[a] | r[a + 1] << 8
    d3 = r[0xD3] | r[0xD4] << 8 | r[0xD5] << 16
    d6 = r[0xD6]
    m32 = ((r[0xD5] if d6 == 0x80 else d6) << 16) | w(0x1B11)
    return dict(layout=d3, map32=m32, l1x=w(0x88B), l1y=w(0x895), xmin=w(0x1B21), xmax=w(0x1B23),
                stream=w(0x1B0F), fx=w(0x1B15))


def tile_entry_h(rom, p, tx, ty):
    """horizontal levels: column-major, 16 chunk rows."""
    cx, cy = tx >> 2, ty >> 2
    c = rom.u16(p['layout'] + (cx * 16 + cy) * 2)
    ci = c & 0x3FFF
    sx, sy = tx & 3, ty & 3
    if c & 0x4000: sx = 3 - sx
    if c & 0x8000: sy = 3 - sy
    e = rom.u16(p['map32'] + ci * 32 + (sy * 4 + sx) * 2)
    return e ^ (c & 0xC000)


def vram_entry(vram, sc, scs, tx, ty):
    scr = 0
    if scs & 1: scr += (tx >> 5) & 1
    if scs & 2: scr += ((ty >> 5) & 1) * (2 if scs & 1 else 1)
    a = (sc + scr * 0x400 + (ty & 31) * 32 + (tx & 31)) * 2
    return vram[a] | vram[a + 1] << 8


def check(rom, e):
    p = level_params(e)
    vram = open('%s/%s/vram.bin' % (S, e), 'rb').read()
    bg = open('%s/%s/bg.bin' % (S, e), 'rb').read()
    sc, h, v, size, _, name, scs = struct.unpack_from('<HHHBBHH', bg, 0)
    ok = tot = 0
    bad = []
    for ty in range(v // 8 + 1, v // 8 + 27):
        for tx in range(h // 8 + 1, h // 8 + 31):
            want = vram_entry(vram, sc, scs, tx, ty)
            got = tile_entry_h(rom, p, tx, ty) if p['stream'] == 0x800D else None
            tot += 1
            if got == want:
                ok += 1
            elif len(bad) < 3:
                bad.append((tx, ty, want, got))
    return p, ok, tot, bad


if __name__ == "__main__":
    rom = Rom(sys.argv[1])
    for e in sys.argv[2:]:
        p, ok, tot, bad = check(rom, e)
        print(e, 'stream %04x layout %06x map32 %06x L1=(%d,%d) match %d/%d' % (p['stream'], p['layout'], p['map32'], p['l1x'], p['l1y'], ok, tot),
              ' '.join('(%d,%d %04x vs %s)' % (b[0], b[1], b[2], '%04x' % b[3] if b[3] is not None else '-') for b in bad))
