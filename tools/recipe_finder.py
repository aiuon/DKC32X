"""Research tool: trace each level's VRAM/CGRAM contents (from SNES dumps) back to ROM assets."""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '.'))
import paths
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dkcrom import Rom
from asset_index import load_assets

SURVEY = paths.SURVEY
LEVELS = ['16', '0C', '17', '0D', 'BF', '3E', 'DE', '22', 'D9', '2E', '07', '31', '42', 'A5', 'A4', 'D0', '43',
          '24', '6D', 'A7', '14', 'CE', '40', '2F', '18', '27', '41', '30', '12', '0A', '36', '2B', '68', 'E0',
          'E1', 'E2', 'E3', 'E4', 'E5', 'EA', 'E6', 'E7', 'E8', 'E9', 'EC', 'ED']


def find_runs(hay, assets, minlen=64, step=64):
    """Greedy cover of hay with asset slices. Returns list of (hay_off, length, asset_name, asset_off)."""
    out = []
    pos = 0
    n = len(hay)
    while pos < n:
        blk = hay[pos:pos + minlen]
        if len(blk) < minlen or not any(blk):
            pos += step
            continue
        best = None
        for cat, name, s, e, data in assets:
            i = data.find(blk)
            while i >= 0:
                L = minlen
                while pos + L < n and i + L < len(data) and hay[pos + L] == data[i + L]:
                    L += 1
                if best is None or L > best[1]:
                    best = (pos, L, name, i)
                i = data.find(blk, i + 1)
                if best and best[1] > 4096:
                    break
        if best:
            out.append(best)
            pos += best[1]
            pos = (pos + 1) & ~1
        else:
            pos += step
    return out


def main(rom_path, which=None):
    rom = Rom(rom_path)
    assets = load_assets(rom)
    gfx = [a for a in assets if a[0] in ("CompressedGFX", "UncompressedGFX", "CompressedTilemaps",
                                         "UncompressedTilemaps", "Map32")]
    pals = [a for a in assets if a[0] == "Palettes"]
    for e in (which or LEVELS):
        d = os.path.join(SURVEY, e)
        if not os.path.exists(d + "/vram.bin"):
            continue
        vram = open(d + "/vram.bin", "rb").read()
        cg = open(d + "/cgram.bin", "rb").read()
        bg = open(d + "/bg.bin", "rb").read()
        print("=== entrance", e)
        for i in range(3):
            sc, h, v, size, _, name, scs = struct.unpack_from('<HHHBBHH', bg, i * 12)
            print("  BG%d map %04x size %d chars %04x" % (i + 1, sc, scs, name))
        for pos, L, nm, off in find_runs(vram, gfx):
            print("  VRAM w%04x-w%04x <- %s +%x" % (pos // 2, (pos + L) // 2, nm, off))
        for pos, L, nm, off in find_runs(cg, pals, minlen=16, step=2):
            print("  CGRAM %3d-%3d <- %s +%x" % (pos // 2, (pos + L) // 2, nm, off))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2:] or None)
