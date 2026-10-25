"""Research step: produce tools/levels.json (names/addresses/numbers only, no game data) from the
SNES survey dumps.  The build tool replays these recipes against the user's ROM."""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '.'))
import paths
import sys, os, struct, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dkcrom import Rom
from asset_index import load_assets
from recipe_finder import find_runs
from levelgen import bg_info, map_words
import validate_layout as V

S = paths.SURVEY
WORLDS = [
    ('Kongo Jungle', 'EC', [('16', 'Jungle Hijinxs'), ('0C', 'Ropey Rampage'), ('01', 'Reptile Rumble'), ('BF', 'Coral Capers'), ('17', 'Barrel Cannon Canyon'), ('E0', "Very Gnawty's Lair")]),
    ('Monkey Mines', 'ED', [('D9', "Winky's Walkway"), ('2E', 'Mine Cart Carnage'), ('07', 'Bouncy Bonanza'), ('31', 'Stop & Go Station'), ('42', 'Millstone Mayhem'), ('E1', "Necky's Nuts")]),
    ('Vine Valley', 'E9', [('A5', 'Vulture Culture'), ('A4', 'Tree Top Town'), ('D0', 'Forest Frenzy'), ('43', 'Temple Tempest'), ('0D', 'Orang-utan Gang'), ('DE', 'Clam City'), ('E5', 'Bumble B Rumble')]),
    ('Gorilla Glacier', 'E8', [('24', 'Snow Barrel Blast'), ('6D', 'Slipslide Ride'), ('A7', 'Ice Age Alley'), ('3E', 'Croctopus Chase'), ('14', 'Torchlight Trouble'), ('CE', 'Rope Bridge Rumble'), ('E2', 'Really Gnawty Rampage')]),
    ('Kremkroc Industries', 'E7', [('40', 'Oil Drum Alley'), ('2F', 'Trick Track Trek'), ('18', 'Elevator Antics'), ('22', 'Poison Pond'), ('27', 'Mine Cart Madness'), ('41', 'Blackout Basement'), ('E3', 'Boss Dumb Drum')]),
    ('Chimp Caverns', 'E6', [('30', 'Tanked Up Trouble'), ('12', 'Manic Mincers'), ('0A', 'Misty Mine'), ('36', 'Loopy Lights'), ('2B', 'Platform Perils'), ('E4', "Necky's Revenge")]),
    ('Gang-Plank Galleon', None, [('68', 'Gang-Plank Galleon')]),
]
# rooms reached only from inside a level (bonus rooms, animal bonus stages): built like levels, not
# listed in the level menu.  ALIASES: other entrances of a room (exits from bonus rooms, checkpoint
# barrels) - the same layout region, so the same pack level.
ROOMS = [
    ('06', 'JH Bonus 1', 'Kongo Jungle'), ('1A', 'JH Bonus 2', 'Kongo Jungle'),
    ('1C', 'Ropey Bonus 1', 'Kongo Jungle'), ('1B', 'Ropey Bonus 2', 'Kongo Jungle'),
    ('02', 'Reptile Bonus 1', 'Kongo Jungle'), ('48', 'Reptile Bonus 2', 'Kongo Jungle'),
    ('04', 'Reptile Bonus 3', 'Kongo Jungle'),
    ('46', 'Barrel Bonus 1', 'Kongo Jungle'), ('5D', 'Barrel Bonus 2', 'Kongo Jungle'),
    ('D2', 'Rambi Bonus', 'Kongo Jungle'), ('A6', 'Enguarde Bonus', 'Kongo Jungle'),
    ('D3', 'Winky Bonus', 'Kongo Jungle'), ('6C', 'Expresso Bonus', 'Kongo Jungle'),
    ('47', "Kong's Banana Hoard", 'Kongo Jungle'),   # (4C, the full hoard after the last boss: BG2 pile TODO)
]
ALIASES = {
    '16': ['08', '6E', '0E', '5F', '25'], '0C': ['6F', '70', '0F'], '01': ['EA', '09', 'C9', '0B', '26'],
    'BF': ['C0'], '17': ['76', '77', '19'],
}
# the other worlds' rooms / aliases from the disassembly's entrance names (Misc_Defines_DKC1.asm):
# <Level>_Enter* = a room of that level, <Level>_Exit* / _CheckpointBarrel / _Warp / _Main<n> = the
# level itself (aliases).  World 1 keeps the hand-made lists above (+ the animal rooms, the hoard).
def auto_rooms():
    import re
    D = os.path.join(paths.DISASM, 'Misc_Defines_DKC1.asm')
    ents = {}
    for ln in open(D, encoding='latin-1'):
        m = re.match(r'!Define_DKC1_EntranceID_(\w+?)_(\w+)\s*=\s*\$00([0-9A-F]{2})', ln)
        if m: ents.setdefault(m.group(1).lower().replace('&', ''), []).append((m.group(2), m.group(3)))
    rooms, aliases = [], {}
    for world, mapent, levels in WORLDS[1:]:
        for e, name in levels:
            key = None
            for k, lst in ents.items():
                if any(kind.startswith('Main') and v == e for kind, v in lst): key = k
            if key is None: continue
            for kind, v in ents[key]:
                if v == e: continue
                if kind.startswith('Enter'):
                    rooms.append((v, '%s %s' % (name, kind[5:]), world))
                elif kind.startswith(('Exit', 'Checkpoint', 'Warp', 'Main')):
                    aliases.setdefault(e, []).append(v)
    return rooms, aliases


NMI_TYPES = {}


def nmi_table():
    L = open(os.path.join(paths.DISASM, 'Routine_Macros_DKC1.asm')).read().split('\n')
    i = [k for k, l in enumerate(L) if l.startswith('DATA_80C5BD:')][0]
    import re
    tab = []
    k = i + 1
    while L[k].strip().startswith('dw'):
        tab.append(re.findall(r'CODE_(\w+)', L[k])[0]); k += 1
    return tab


def regs(e):
    r = open('%s/%s/regs.bin' % (S, e), 'rb').read()
    ppu = open('%s/%s/ppu.bin' % (S, e), 'rb').read()
    return dict(TM=r[0x212c], TS=r[0x212d], CGWSEL=r[0x2130], CGADSUB=r[0x2131],
                fixed=[ppu[2776], ppu[2777], ppu[2778]], W12SEL=r[0x2123], W34SEL=r[0x2124],
                WOBJSEL=r[0x2125], TMW=r[0x212e], TSW=r[0x212f], BGMODE=ppu[64], BG3PRIO=ppu[65])


def main():
    rom = Rom(sys.argv[1])
    assets = load_assets(rom)
    gfx = [a for a in assets if a[0] in ("CompressedGFX", "UncompressedGFX", "CompressedTilemaps", "UncompressedTilemaps")]
    pals = [a for a in assets if a[0] == "Palettes"]
    nmi = nmi_table()
    out = []
    todo = [(world, e, name, False) for world, mapent, levels in WORLDS for e, name in levels]
    ar, aa = auto_rooms()
    have = set(e for e, _, _ in ROOMS)
    rooms = ROOMS + [r for r in ar if r[0] not in have and os.path.exists('%s/%s/ppu.bin' % (S, r[0]))]
    for r in ar:
        if not os.path.exists('%s/%s/ppu.bin' % (S, r[0])): print('room %s %s: no survey (survey/run.sh %s)' % (r[0], r[1], r[0]))
    for k, v in aa.items(): ALIASES.setdefault(k, v)
    todo += [(world, e, name, True) for e, name, world in rooms]
    for world, e, name, room in todo:
        if True:
            d = '%s/%s' % (S, e)
            vram = open(d + '/vram.bin', 'rb').read()
            cg = open(d + '/cgram.bin', 'rb').read()
            bgs = bg_info(e)
            # keep only runs touching BG chars/maps
            want = bytearray(65536)
            for i, L in enumerate(bgs):
                if L['sc'] == 0 and L['name'] == 0:
                    continue
                for k in range(map_words(L['scs']) * 2):
                    want[(L['sc'] * 2 + k) & 0xFFFF] = 1
                tsz = 32 if L['bpp'] == 4 else 16
                for k in range(1024 * tsz):
                    want[(L['name'] * 2 + k) & 0xFFFF] = 1
            runs = [(pos, ln, nm, off) for pos, ln, nm, off in find_runs(vram, gfx)
                    if any(want[pos:pos + ln])]
            cgr = [(pos, ln, nm, off) for pos, ln, nm, off in find_runs(cg[:256], pals, minlen=16, step=2)]
            p = V.level_params(e) if os.path.exists(d + '/ram.bin') else {}
            r = open(d + '/ram.bin', 'rb').read()
            w = lambda a: r[a] | r[a + 1] << 8
            lvl = dict(entrance=int(e, 16), name=name, world=world, nmi=nmi[int(e, 16)] if int(e, 16) < len(nmi) else None,
                       layout=p.get('layout'), map32=p.get('map32'), stream=p.get('stream'),
                       x_min=w(0x1B23), x_max=w(0x1B25), start=[w(0x88B), w(0x895)],
                       bgs=bgs, vram_runs=runs, cgram_runs=cgr, regs=regs(e),
                       uncovered_cgram=[i for i in range(128) if not any(pos // 2 <= i < (pos + ln) // 2 for pos, ln, _, _ in cgr)],
                       room=room, aliases=[int(a, 16) for a in ALIASES.get(e, [])])
            out.append(lvl)
            print(e, name, 'runs', len(runs), 'cg', len(cgr), 'uncov', len(lvl['uncovered_cgram']))
    json.dump(out, open(os.path.join(os.path.dirname(__file__), 'levels.json'), 'w'), indent=1)


if __name__ == "__main__":
    main()
