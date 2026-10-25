#!/usr/bin/env python3
"""Per-world sprite characters for the all-worlds ROM (mkgame.py WORLD_CHARS).

The DMA logs of the sim runs miss characters that are loaded once at a level's start or that the
runs never reached (Croctopus, Chomps ...).  The level sprite lists list them all: sprdata.json
(level name -> NorSpr type names, from the disassembly's level sprite data) mapped to the
DynamicSprites graphics groups mkgame uses, and grouped by world (tools/levels.json + maps.json).
  world_chars.py SPRDATA.json OUT.json   -> {"1": [names], "2": [...], ...}"""
import json, os, re, sys

D = os.path.dirname(os.path.abspath(__file__))
# NorSpr type names that draw with another character's graphics
ALIAS = {'BlueKrusha': 'Krusha', 'GreyKrusha': 'Krusha', 'FlyingNecky': 'Necky', 'NutThrowingNecky': 'Necky',
         'RollingTire': 'Tire', 'UnknownRollingTire': 'Tire', 'FloatingTire': 'Tire', 'UnknownHalfTire': 'HalfTire',
         'MillstoneGnawty': 'Gnawty'}
# characters that come with another one (projectiles, effects, parts)
WITH = {'Necky': ['NeckyFeather', 'MiniNeckyNut'], 'MiniNecky': ['MiniNeckyNut'], 'MasterNecky': ['MasterNeckysNut'],
        'Tire': ['WhiteTire', 'HalfWhiteTire'], 'OilDrum': ['OilFire', 'SmallFire'], 'Minecart': ['MinecartSparks'],
        'Clambo': ['Pearl'], 'Krusha': [], 'KRool': ['KRoolCrown', 'KRoolsShip', 'ThrownCane']}


EXTRA = {1: ['VeryGnawty', 'ClownFish', 'JunglePlant'], 2: ['MasterNecky', 'MasterNeckysNut', 'Krash', 'MinePlatform', 'CavePlatform'],
         3: ['QueenB', 'ClownFish', 'MasterNecky', 'MasterNeckysNut'], 4: ['VeryGnawty', 'ClownFish', 'CavePlatform'],
         5: ['DumbDrum', 'Krash', 'SmallFire', 'OilFire', 'TrickTrackTrekPlatform', 'FactoryPlatform', 'FueledPlatform',
             'CavePlatform'],
         6: ['MasterNecky', 'MasterNeckysNut', 'FueledPlatform', 'ArrowPlatform', 'MinePlatform'],
         7: ['KRool', 'KRoolCrown', 'KRoolsShip', 'ThrownCane', 'DKIslandInFinalBattle']}


def key(s):
    return re.sub(r'[^a-z0-9]', '', s.lower().replace('&', 'and'))


def main():
    sp = json.load(open(sys.argv[1]))
    levels = json.load(open(os.path.join(D, 'levels.json')))
    maps = json.load(open(os.path.join(D, 'maps.json')))
    wnum = dict((m['name'], i + 1) for i, m in enumerate(maps))
    lw = {}
    for lv in levels:
        if lv.get('world') in wnum:
            lw[key(lv['name'])] = wnum[lv['world']]
    out, unknown = {}, set()
    for k, names in sp.items():
        lvl = key(k.split('_')[0])
        w = lw.get(lvl, 7 if lvl == 'gangplankgalleon' else None)   # (the Galleon: its own bank, "world 7")
        if w is None:
            unknown.add(k.split('_')[0])
            continue
        s = out.setdefault(w, set())
        for n in names:
            n = ALIAS.get(n, n)
            s.add(n)
            s.update(WITH.get(n, []))
    # characters no level sprite list names (bosses, things spawned by code) -> their worlds
    for w, names in EXTRA.items():
        out.setdefault(w, set()).update(names)
    # (H18) the rock debris of a blasted bonus-room wall: preloaded when a level is entered from a
    # bonus room's exit (CODE_8081C5) in every world - garbage OBJ tiles (bananas) in world 2-6 without
    for w in range(1, 7):
        out.setdefault(w, set()).update(['FlyingRock', 'SmallFlyingRock'])
    json.dump(dict((str(w), sorted(s)) for w, s in sorted(out.items())), open(sys.argv[2], 'w'), indent=1)
    for w, s in sorted(out.items()):
        print('world %d: %d types' % (w, len(s)))
    if unknown:
        print('levels with no world (left out):', ', '.join(sorted(unknown)))


if __name__ == '__main__':
    main()
