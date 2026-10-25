"""Research: find where layout-built background tilemaps come from in the tileset layout strip."""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '.'))
import paths
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dkcrom import Rom
import validate_layout as V
from levelgen import bg_info, map_words

S = paths.SURVEY


def strip_columns(rom, p, ncols, rows=64):
    """flattened 8x8 tile columns of the whole tileset strip"""
    cols = []
    for tx in range(ncols * 4):
        cols.append(tuple(V.tile_entry_h(rom, p, tx, ty) for ty in range(rows)))
    return cols


def vram_col(vram, L, tx):
    scs = L['scs']
    hgt = 64 if scs & 2 else 32
    return [V.vram_entry(vram, L['sc'], scs, tx, ty) for ty in range(hgt)]


def search(rom, e, layer, ncols):
    p = V.level_params(e)
    vram = open('%s/%s/vram.bin' % (S, e), 'rb').read()
    L = bg_info(e)[layer - 1]
    wid = 64 if L['scs'] & 1 else 32
    cols = strip_columns(rom, p, ncols)
    # index by first 4 non-trivial entries
    res = []
    for tx in range(wid):
        c = vram_col(vram, L, tx)
        found = None
        for ry in range(0, len(c) - 8, 4):
            key = tuple(c[ry:ry + 8])
            if len(set(key)) < 3:
                continue
            for sx, col in enumerate(cols):
                for sy in range(0, 64 - 8):
                    if col[sy:sy + 8] == key:
                        found = (sx, sy - ry)
                        break
                if found: break
            if found: break
        res.append(found)
    return res


if __name__ == "__main__":
    rom = Rom(sys.argv[1])
    e, layer, ncols = sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    r = search(rom, e, layer, ncols)
    print(e, 'BG%d' % layer, r)
