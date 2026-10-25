"""Virtual SNES VRAM/CGRAM for a level, rebuilt purely from the user's ROM using the recipes in
levels.json (asset names + offsets) and the layout-built background rules below."""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '.'))
import paths
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dkcrom import Rom, decode_4bpp_tile, decode_2bpp_tile  # noqa
from asset_index import load_assets

HERE = os.path.dirname(os.path.abspath(__file__))

# layout-built background tilemaps: layout base -> (layer, tile col fn, tile row offset, w, h)
LAYOUT_BG = {
    0xD90000: dict(layer=2, w=64, h=32, col=lambda c: 672 + ((c + 32) & 63), row0=0),    # jungle
    0xC36200: dict(layer=2, w=32, h=32, col=lambda c: c, row0=0),                        # forest
    0xDA0000: dict(layer=2, w=32, h=32, col=lambda c: c, row0=0),                        # cave
    0xE03636: dict(layer=2, w=64, h=32, col=lambda c: c, row0=0),                        # snow
    0xE20000: dict(layer=1, w=64, h=64, col=lambda c: c, row0=0),                        # factory fg
    0xC3EDA0: dict(layer=2, w=32, h=32, col=lambda c: c, row0=32),                       # bosses
    0xE30000: dict(layer=2, w=32, h=32, col=lambda c: c, row0=32),                       # galleon
    0xE101E5: dict(layer=2, w=64, h=32, col=lambda c: c, row0=0),                        # crystal (CODE_B992B2)
}


class Levels:
    def __init__(self, rom):
        self.rom = rom
        self.levels = json.load(open(os.path.join(HERE, 'levels.json')))
        self.assets = {a[1]: a for a in load_assets(rom)}

    def by_entrance(self, e):
        for l in self.levels:
            if l['entrance'] == e:
                return l
        raise KeyError(e)


def layout_tile(rom, lv, tx, ty, base=None):
    """SNES tilemap word for playfield tile (tx,ty). Horizontal: column-major 16 chunk rows;
    vertical (underwater/crystal): row-major 64 chunks wide."""
    lay = base if base is not None else lv['layout']
    cx, cy = tx >> 2, ty >> 2
    if lv['stream'] in (0x8168, 0x831B):
        idx = cy * 64 + cx
    else:
        idx = cx * 16 + cy
    c = rom.u16(lay + idx * 2)
    ci = c & 0x3FFF
    sx, sy = tx & 3, ty & 3
    if c & 0x4000:
        sx = 3 - sx
    if c & 0x8000:
        sy = 3 - sy
    e = rom.u16(lv['map32'] + ci * 32 + (sy * 4 + sx) * 2)
    return e ^ (c & 0xC000)


def map_addr(sc, scs, tx, ty):
    scr = 0
    if scs & 1:
        scr += (tx >> 5) & 1
    if scs & 2:
        scr += ((ty >> 5) & 1) * (2 if scs & 1 else 1)
    return (sc + scr * 0x400 + (ty & 31) * 32 + (tx & 31)) & 0x7FFF


def build_vram(L, lv):
    rom = L.rom
    vram = bytearray(65536)
    for pos, ln, name, off in lv['vram_runs']:
        # 'ROM:xxxxxx' = raw ROM bytes at that SNES address (DMAs that read past an asset's end)
        data = rom.read(int(name[4:], 16), off + ln) if name.startswith('ROM:') else L.assets[name][4]
        chunk = data[off:off + ln]
        vram[pos:pos + len(chunk)] = chunk
    rule = LAYOUT_BG.get(lv['layout'])
    if rule:
        bg = lv['bgs'][rule['layer'] - 1]
        for ty in range(rule['h']):
            for tx in range(rule['w']):
                e = layout_tile(rom, lv, rule['col'](tx), rule['row0'] + ty)
                a = map_addr(bg['sc'], bg['scs'], tx, ty) * 2
                vram[a] = e & 0xFF
                vram[a + 1] = e >> 8
    cg = bytearray(512)
    for pos, ln, name, off in lv['cgram_runs']:
        data = L.assets[name][4]
        chunk = data[off:off + ln]
        cg[pos:pos + len(chunk)] = chunk
    return vram, cg


def map_words(scs):
    return 0x400 * (1 + (scs & 1)) * (1 + ((scs >> 1) & 1))


def verify(L, lv, survey=None):
    survey = survey or paths.SURVEY
    """research: compare rebuilt VRAM against the SNES dump on all static BG data"""
    e = '%02X' % lv['entrance']
    dv = open('%s/%s/vram.bin' % (survey, e), 'rb').read()
    dc = open('%s/%s/cgram.bin' % (survey, e), 'rb').read()
    vram, cg = build_vram(L, lv)
    res = []
    play = 2 if lv['layout'] == 0xE20000 else 1
    for i, bg in enumerate(lv['bgs']):
        if bg['sc'] == 0 and bg['name'] == 0:
            continue
        n = map_words(bg['scs'])
        if i + 1 == play:
            mapok = None
        else:
            mapok = sum(vram[(bg['sc'] * 2 + k) & 0xFFFF] == dv[(bg['sc'] * 2 + k) & 0xFFFF] for k in range(n * 2)) / (n * 2)
        tiles = set()
        src = dv
        for k in range(n):
            a = (bg['sc'] + k) * 2 & 0xFFFF
            tiles.add((src[a] | src[a + 1] << 8) & 0x3FF)
        tsz = 32 if bg['bpp'] == 4 else 16
        idx = [((bg['name'] * 2) + t * tsz + k) & 0xFFFF for t in tiles for k in range(tsz)]
        chok = sum(vram[a] == dv[a] for a in idx) / max(1, len(idx))
        res.append((i + 1, mapok, chok))
    palok = sum(cg[k] == dc[k] for k in range(256)) / 256
    return res, palok


if __name__ == "__main__":
    rom = Rom(sys.argv[1])
    L = Levels(rom)
    for lv in L.levels:
        res, palok = verify(L, lv)
        print('%02X %-22s' % (lv['entrance'], lv['name']),
              ' '.join('BG%d map %s chr %3.0f%%' % (b, '  - ' if m is None else '%3.0f%%' % (m * 100), c * 100) for b, m, c in res),
              ' pal %3.0f%%' % (palok * 100))
