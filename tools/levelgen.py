"""Research: derive per-level VRAM recipes (asset copies) from SNES dumps and check coverage
of every BG layer's chars + tilemap.  Output is used to write tools/levels_data.py."""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '.'))
import paths
import sys, os, struct, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dkcrom import Rom
from asset_index import load_assets
from recipe_finder import find_runs
import validate_layout as V

S = paths.SURVEY


def bg_info(e):
    bg = open('%s/%s/bg.bin' % (S, e), 'rb').read()
    out = []
    for i in range(3):
        sc, h, v, size, _, name, scs = struct.unpack_from('<HHHBBHH', bg, i * 12)
        out.append(dict(sc=sc, name=name, scs=scs, h=h, v=v, bpp=4 if i < 2 else 2))
    return out


def map_words(scs):
    return 0x400 * (1 + (scs & 1)) * (1 + ((scs >> 1) & 1))


def used_tiles(vram, L):
    """tile numbers referenced by a BG tilemap"""
    n = map_words(L['scs'])
    t = set()
    for i in range(n):
        a = (L['sc'] + i) * 2 & 0xFFFF
        e = vram[a] | vram[a + 1] << 8
        t.add(e & 0x3FF)
    return t


def analyse(rom, assets, e):
    vram = open('%s/%s/vram.bin' % (S, e), 'rb').read()
    bgs = bg_info(e)
    gfx = [a for a in assets if a[0] in ("CompressedGFX", "UncompressedGFX", "CompressedTilemaps",
                                         "UncompressedTilemaps", "Map32")]
    runs = find_runs(vram, gfx)
    covered = bytearray(65536)
    for pos, L, nm, off in runs:
        for k in range(pos, min(pos + L, 65536)):
            covered[k] = 1
    rep = []
    for i, L in enumerate(bgs):
        if L['sc'] == 0 and L['name'] == 0:
            continue
        mw = map_words(L['scs'])
        ma = L['sc'] * 2
        mcov = sum(covered[(ma + k) & 0xFFFF] for k in range(mw * 2)) / (mw * 2)
        tiles = used_tiles(vram, L)
        tsz = 32 if L['bpp'] == 4 else 16
        ca = L['name'] * 2
        need = [(ca + t * tsz + k) & 0xFFFF for t in tiles for k in range(tsz)]
        nz = [a for a in need if vram[a]]
        ccov = sum(covered[a] for a in nz) / max(1, len(nz))
        rep.append((i + 1, mcov, ccov))
    return runs, rep


if __name__ == "__main__":
    rom = Rom(sys.argv[1])
    assets = load_assets(rom)
    for e in sys.argv[2:]:
        runs, rep = analyse(rom, assets, e)
        print(e, ' '.join('BG%d map %.0f%% chars %.0f%%' % (b, m * 100, c * 100) for b, m, c in rep))
