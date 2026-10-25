"""Research step: recipes for the six world map screens (entrances E6..ED) -> tools/maps.json"""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '.'))
import paths
import sys, os, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dkcrom import Rom
from asset_index import load_assets
from recipe_finder import find_runs
from levelgen import bg_info
S = paths.SURVEY
MAPS = [('EC', 'Kongo Jungle'), ('ED', 'Monkey Mines'), ('E9', 'Vine Valley'), ('E8', 'Gorilla Glacier'),
        ('E7', 'Kremkroc Industries'), ('E6', 'Chimp Caverns')]
rom = Rom(sys.argv[1])
assets = load_assets(rom)
gfx = [a for a in assets if a[0] in ("CompressedGFX", "UncompressedGFX", "CompressedTilemaps", "UncompressedTilemaps")]
pals = [a for a in assets if a[0] == "Palettes"]
out = []
for e, name in MAPS:
    d = '%s/%s' % (S, e)
    vram = open(d + '/vram.bin', 'rb').read(); cg = open(d + '/cgram.bin', 'rb').read()
    bgs = bg_info(e)
    b1 = bgs[0]
    lo, hi = b1['name'] * 2, b1['name'] * 2 + 1024 * 32
    mlo, mhi = b1['sc'] * 2, b1['sc'] * 2 + 2048
    runs = [r for r in find_runs(vram, gfx) if (r[0] < hi and r[0] + r[1] > lo) or (r[0] < mhi and r[0] + r[1] > mlo)]
    cgr = find_runs(cg[:256], pals, minlen=16, step=2)
    out.append(dict(entrance=int(e, 16), name=name, bgs=bgs, vram_runs=runs, cgram_runs=cgr))
    print(e, name, len(runs), len(cgr))
json.dump(out, open(os.path.join(os.path.dirname(__file__), 'maps.json'), 'w'), indent=1)
