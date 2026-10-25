"""Index every graphics/tilemap/palette asset listed in the disassembly's asset table,
so level VRAM/CGRAM contents can be traced back to ROM sources (research tool)."""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '.'))
import paths
import re
import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dkcrom import Rom

ASSET_ASM = os.path.join(paths.DISASM, 'AsarScripts/AssetPointersAndFiles.asm')


def load_assets(rom):
    import layout
    assets = []
    for cat, name, s, e in layout.assets():
        if True:
            if cat.startswith("Compressed"):
                try:
                    data = rom.decompress(s)
                except Exception:
                    data = b""
            else:
                data = rom.read(s, e - s)
            assets.append((cat, name, s, e, data))
    return assets


if __name__ == "__main__":
    rom = Rom(sys.argv[1])
    a = load_assets(rom)
    from collections import Counter
    print(Counter(x[0] for x in a))
    for x in a:
        if x[0] in ("CompressedGFX", "CompressedTilemaps", "Palettes"):
            print(x[0], x[1], hex(x[2]), len(x[4]))
