#!/usr/bin/env python3
"""mkgame: build-time data for the game logic on the 32X.

  mkgame.py ROM memprofile OUTDIR snap1.wai[:entrance] ...

memprofile: lines 'R <1KB page>' (SNES ROM pages the game code reads, incl. interpreted code) and
'W <256-byte page>' (high WRAM pages), from hostrun (MEMTRACK).
Writes OUTDIR/snes_blob.bin (the needed SNES ROM pages, with whole disassembly data blocks),
OUTDIR/game_tab.c (page tables, WRAM pool, snapshot table) and OUTDIR/snaps.bin.
Personal-use data: the snapshots come from snes9x save states.
"""
import sys, os, re, struct, bisect
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import disparse
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import layout

def lz_pack(d):
    """byte LZ: t < 0x80: t + 1 literals follow; else match of (t & 0x7F) + 3 bytes at distance
    (2 bytes BE, 1..65535) back in the output (game.c lz_unpack)"""
    out = bytearray(); lit = bytearray(); i = 0; n = len(d)
    heads = {}
    def flush():
        while lit:
            k = min(len(lit), 128); out.append(k - 1); out.extend(lit[:k]); del lit[:k]
    while i < n:
        best = 0; bd = 0
        if i + 3 <= n:
            key = d[i:i + 3]
            for j in reversed(heads.get(key, [])[-64:]):
                if i - j > 65535: break
                l = 3
                while l < 130 and i + l < n and d[j + l] == d[i + l]: l += 1
                if l > best: best, bd = l, i - j
                if l == 130: break
        if best >= 3:
            flush(); out.append(0x80 | (best - 3)); out += bytes([bd >> 8, bd & 0xFF])
            for k in range(i, i + best):
                if k + 3 <= n: heads.setdefault(d[k:k + 3], []).append(k)
            i += best
        else:
            if i + 3 <= n: heads.setdefault(d[i:i + 3], []).append(i)
            lit.append(d[i]); i += 1
    flush()
    return bytes(out)

def lz_unpack(z, n):
    o = bytearray(); i = 0
    while len(o) < n:
        t = z[i]; i += 1
        if t < 0x80: o += z[i:i + t + 1]; i += t + 1
        else:
            l = (t & 0x7F) + 3; dist = z[i] << 8 | z[i + 1]; i += 2
            for _ in range(l): o.append(o[-dist])
    return bytes(o)

# ---- world banks: pages only one world's levels need go to that world's 512 KB bank (32X cart
# window 6, with its own copy of the snes_pg table), the rest stays resident.
COMMON_CHARS = set('''DonkeyKong Diddy Rambi Expresso Winky Enguarde Squawks CrankyKong CandyKong FunkyKong
FunkysSurfboard AnimalBuddyToken LargeAnimalBuddyToken AnimalBuddyBox HUDBalloon LifeBalloon EarlyLifeBalloon
GoldenLetters MapDonkeyKong MapDiddy InactiveMapDonkeyKong InactiveMapDiddy MapAirplaneBarrel AirplaneBarrel Barrel
DKBarrel BarrelPiece SmallBarrelPiece BurstEffect SmokePuff Sparkle DiddyStars CheckpointBarrel CheckpointStars
BananaBunch MovingSingleBanana DiddysHat Timer Exit Sign BarrelCannon TNTBarrel StopBarrel GoBarrel OnBarrel
OffBarrel EnemySpawnBarrel UnknownEffect UnknownSmokeEffect GiantBanana Explosion BlownKiss
Phonograph PhonographHorn BrokenPhonographBoard BoomBox'''.split())   # (+ the cabins' props)
DUP = []                                     # (I16: world_split)
COMMON_FILES = ('Graphics/GFX_Sprite', 'SPC700/', 'Graphics/GFX_Layer3', 'LevelData/BananaFormations')


def world_split(need, insns, starts, binpg, groups):
    """WORLD_PROFILES = 'K:cpu_profile:dma_log,...' (K = world index; cpu profile R pages from hostrun
    MEMTRACK, dma log = sim3 DMALOG: ROM ranges uploaded as OBJ graphics) -> per page: bit 7 resident,
    bit k = in world bank k.  BLOB_FILES entries 'K:path' are world K's, plain ones every world's."""
    import re
    fil = {}                                  # page -> incbin paths covering it
    for lab, path, _ in layout.incbins():
        j = bisect.bisect_right(starts, lab)
        end = starts[j] if j < len(starts) else lab + 0x400
        for q in range(lab >> 10, ((end - 1) >> 10) + 1): fil.setdefault(q, set()).add(path)
    filepages = {}
    for q, fs in fil.items():
        for f in fs: filepages.setdefault(f, set()).add(q)
    charof = {}                               # sprite file path -> character
    for f in filepages:
        m = re.match(r'Graphics/DynamicSprites/GFX_(.+?)\.bin', f)
        if m: charof[f] = re.sub(r'\d+$', '', m.group(1)).rstrip('_').split('_')[0]
    iv = sorted((a, b, n) for n, bl in groups.items() for a, b in bl)
    ivs = [t[0] for t in iv]
    codep = set((a & 0x3FFFFF) >> 10 for a in insns)
    worlds = []
    for item in os.environ['WORLD_PROFILES'].split(','):
        k, cpu, dma = item.split(':')
        rp = set(int(l.split()[1], 16) for l in open(cpu) if l.startswith('R'))
        chars = set()
        for l in open(dma):
            a = int(l.split()[0], 16) & 0x3FFFFF
            j = bisect.bisect_right(ivs, a) - 1
            if j >= 0 and iv[j][0] <= a < iv[j][1]: chars.add(iv[j][2])
        # WORLD_CHARS (tools/world_chars.py): the characters in the world's level sprite lists - the DMA
        # logs miss the ones loaded once at a level's start or never reached by the runs
        wcf = os.environ.get('WORLD_CHARS')
        if wcf:
            import json
            chars |= set(json.load(open(wcf)).get(str(int(k)), [])) & set(groups)
        # (a file is the world's when 2+ of its pages were read, or a small one was: stray reads of a
        #  level file's first page happen in other worlds' levels too)
        files = set(f for f in filepages if not f in charof and
                    len(filepages[f] & rp) >= min(2, len(filepages[f])))
        files |= set(f for f in filepages if f in charof and charof[f] in chars)
        for e in os.environ.get('BLOB_FILES', '').split(','):
            if not e: continue
            kk, _, path = e.rpartition(':') if ':' in e else ('', '', e)
            if kk and int(kk) != int(k): continue
            files |= set(f for f in filepages if f == path or (path.endswith('/') and f.startswith(path)))
        worlds.append((int(k), files, chars))
    pg = [0] * 4096
    for p in need:
        fs = fil.get(p, set())
        if p in codep or binpg.get(p, 0) < 0x400 or any(charof.get(f) in COMMON_CHARS or f.startswith(COMMON_FILES) for f in fs):
            pg[p] = 0x80
            continue
        for wi, (k, files, chars) in enumerate(worlds):
            if fs & files: pg[p] |= 1 << wi
    # WORLD_RESIDENT = comma list of page profiles ('R ppp' lines) read from every world (the animal
    # token rooms, entered from any world's levels): those pages are resident, never banked
    forced = 0
    for f in os.environ.get('WORLD_RESIDENT', '').split(','):
        if not f: continue
        for l in open(f):
            if l.startswith('R'):
                q = int(l.split()[1], 16)
                if q in need and pg[q] != 0x80: pg[q] = 0x80; forced += 1
    if forced: print('mkgame: %d pages resident (WORLD_RESIDENT)' % forced)
    # pages no world claims (characters the DMA logs missed, files read outside the profiles):
    # resident - safe (WORLD_UNCLAIMED=drop: leave them out, to measure)
    unclaimed = [p for p in need if not pg[p]]
    if os.environ.get('WORLD_UNCLAIMED') != 'drop':
        for p in unclaimed: pg[p] = 0x80
        print('mkgame: %d unclaimed pages made resident' % len(unclaimed))
    for wi, (k, files, chars) in enumerate(worlds):
        n = sum(1 for p in need if pg[p] & (1 << wi))
        print('mkgame: world bank %d (world %d): %d KB, %d characters' % (wi, k, n, len(chars - COMMON_CHARS)))
        if n > 496 or os.environ.get('WORLD_VERBOSE'):
            import collections
            cnt = collections.Counter()
            for p in need:
                if pg[p] & (1 << wi):
                    for f in fil.get(p, ()): cnt['spr:' + charof[f] if f in charof else f] += 1
            for f, c in cnt.most_common(40): print('   %4d %s' % (c, f))
        assert n <= (427 if wi == 0 else 496) or os.environ.get("WORLD_NOCHECK"), 'world bank %d over its size' % wi
    # (I16) DUP: resident sprite-graphics pages moved into every world bank, at the same offset in each
    # (right after the page table): window 6 always shows one of the world banks, so such a page is
    # always visible at one fixed address - the same as resident - and its resident ROM goes to
    # translated game code.  As many as the fullest world bank has room for (DUP_MARGIN pages spare).
    global DUP
    DUP = []
    if os.environ.get('WORLD_DUP', '1') != '0':
        nw = len(worlds)
        full = max(sum(1 for p in need if pg[p] & (1 << wi)) for wi in range(nw))
        # (I17) bank 0 (window 6 at power-on) also holds the SDRAM code image: 427 pages there
        lim = [427 if wi == 0 else 496 for wi in range(nw)]
        room = min(lim[wi] - sum(1 for p in need if pg[p] & (1 << wi)) for wi in range(nw)) - int(os.environ.get('DUP_MARGIN', '2'))
        cand = [p for p in need if pg[p] == 0x80 and p not in codep and binpg.get(p, 0) >= 0x400 and fil.get(p)
                and all(charof.get(f) in COMMON_CHARS for f in fil[p])]
        DUP = cand[:max(0, room)]
        for p in DUP: pg[p] = (1 << nw) - 1
        print('mkgame: %d common sprite pages in every world bank (fullest bank %d pages)' % (len(DUP), full))
    print('mkgame: resident pages %d KB (%d pages in no bank: not needed)' % (
        sum(1 for p in need if pg[p] & 0x80), sum(1 for p in need if not pg[p])))
    if os.environ.get('WORLD_VERBOSE'):
        import collections
        cnt = collections.Counter()
        for p in unclaimed:
            if True:
                fs = fil.get(p) or {'(no file: %02X)' % (0xC0 | p >> 6)}
                for f in fs: cnt['spr:' + charof[f] if f in charof else f] += 1
        for f, c in cnt.most_common(30): print('   none %4d %s' % (c, f))
    return pg


def main():
    import rompatch
    rom = rompatch.apply(open(sys.argv[1], 'rb').read())
    prof = sys.argv[2]; out = sys.argv[3]; snaps = sys.argv[4:]
    os.makedirs(out, exist_ok=True)
    rpages, wpages = set(), set()
    for ln in open(prof):
        p = ln.split()
        if len(p) != 2: continue
        if p[0] == 'R': rpages.add(int(p[1], 16))
        elif p[0] == 'W': wpages.add(int(p[1], 16))
    # expand ROM pages to whole disassembly blocks (tables are read in pieces) and keep all code
    _raw = open(sys.argv[1], 'rb').read()
    insns, labels, named, refs = disparse.parse(rom=_raw[0x200:] if len(_raw) % 0x8000 == 0x200 else _raw)
    starts = sorted(set(a & 0x3FFFFF for a in labels.values()))
    need = set(rpages)
    for pg in sorted(rpages):
        a0, a1 = pg << 10, (pg + 1) << 10
        j = bisect.bisect_right(starts, a0) - 1
        while j < len(starts) and (j < 0 or starts[j] < a1):
            if j >= 0:
                s0 = starts[j]; s1 = starts[j + 1] if j + 1 < len(starts) else s0 + 1
                if s1 - s0 <= 0x4000:
                    for q in range(s0 >> 10, ((s1 - 1) >> 10) + 1): need.add(q)
            j += 1
    for a in insns:
        need.add((a & 0x3FFFFF) >> 10)
    # level terrain files (LevelData/*Levels.bin, up to 50 KB): the profile runs only reach the start of
    # each level, the rest of a level in use must be there too (a block with >= 2 pages read)
    import re
    for lab, path, _ in layout.incbins():
        if re.fullmatch(r'LevelData/[A-Za-z]+\.bin', path):
            j = bisect.bisect_right(starts, lab)
            end = starts[j] if j < len(starts) else lab + 0x400
            pages = set(range(lab >> 10, ((end - 1) >> 10) + 1))
            if len(pages & rpages) >= 2: need |= pages
    # whole files: BLOB_FILES (comma list of incbin paths no profile reads - check new levels / rooms
    # with sim_sc MISSING reports; compressed graphics must be complete: a missing page would derail
    # the decompressor) (comma list of incbin paths, e.g. the snow tileset
    # of the Rambi bonus room in world 1, which no profile visits)
    files = [f.rpartition(':')[2] for f in os.environ.get('BLOB_FILES', '').split(',') if f]   # ('K:path': world K's)
    nwf = len(need)
    for lab, path, _ in layout.incbins():
        if True:
            j = bisect.bisect_right(starts, lab)
            end = starts[j] if j < len(starts) else lab + 0x400
            pages = set(range(lab >> 10, ((end - 1) >> 10) + 1))
            if path in files or any(f.endswith('/') and path.startswith(f) for f in files): need |= pages
            lab = None
    if len(need) != nwf: print('mkgame: +%d pages for BLOB_FILES' % (len(need) - nwf))
    # dynamic sprite graphics: all frames of every character with a frame in use
    import re
    groups = {}
    for lab, path, gsz in layout.incbins():
        m = re.fullmatch(r'Graphics/DynamicSprites/GFX_(.+?)\.bin', path)
        if m and gsz:
            name = re.sub(r'\d+$', '', m.group(1)).rstrip('_')
            name = name.split('_')[0]
            j = bisect.bisect_right(starts, lab)
            end = starts[j] if j < len(starts) else lab + gsz + 64
            groups.setdefault(name, []).append((lab, end))
            lab = None
    nchar = 0
    for name, blocks in groups.items():
        used = [(a, b) for a, b in blocks if any(p in rpages for p in range(a >> 10, ((b - 1) >> 10) + 1))]
        npg = sum(((b - 1) >> 10) - (a >> 10) + 1 for a, b in blocks)
        nused = sum(((b - 1) >> 10) - (a >> 10) + 1 for a, b in used)
        # every character's frames: a pose shown only later in a level (a life balloon, an animal
        # buddy) would otherwise read zeros (a 64 KB VRAM DMA of size 0 trashed the OBJ tiles).
        # SPR_EXCLUDE = comma list of characters left out (ROM space)
        if name in os.environ.get('SPR_EXCLUDE', '').split(','):
            continue
        nchar += 1
        for a, b in blocks:
            for q in range(a >> 10, ((b - 1) >> 10) + 1): need.add(q)
    print('mkgame: sprite graphics of %d characters' % nchar)
    # whole banks (BLOB_BANKS: comma list of 64 KB HiROM banks, e.g. C0,F5): the code banks' tables -
    # profiles miss data read only in rare situations (the Necky's nut speeds, DATA_B68EE0, read 0)
    # (only pages holding something besides incbin files: graphics / level files are covered by the
    # profiles and the rules above)
    nb0 = len(need)
    binpg = {}                                   # page -> bytes covered by incbin blocks
    for lab, _, _ in layout.incbins():
        j = bisect.bisect_right(starts, lab)
        end = starts[j] if j < len(starts) else lab + 0x400
        for q in range(lab >> 10, ((end - 1) >> 10) + 1):
            binpg[q] = binpg.get(q, 0) + min(end, (q + 1) << 10) - max(lab, q << 10)
    for b in [int(x, 16) & 0x3F for x in os.environ.get('BLOB_BANKS', '').split(',') if x]:
        for q in range(b << 6, (b + 1) << 6):
            if binpg.get(q, 0) < 0x400: need.add(q)
    if len(need) != nb0: print('mkgame: +%d pages for whole banks' % (len(need) - nb0))
    # BLOB_DROP: comma list of incbin path prefixes whose pages are left out (data the runtime
    # never reads: the SPC uploads are HLE'd and only read their 4-byte block headers; pages where a
    # block starts are kept - the sound code also reads the start of each sample)
    drop = [d for d in os.environ.get('BLOB_DROP', '').split(',') if d]
    keep = set(int(x, 16) for x in os.environ.get('BLOB_KEEP', '').split(',') if x)
    if drop or os.environ.get('GFXSKIP', '1') == '1':
        names = {}
        for lab, path, _ in layout.incbins(): names.setdefault(lab, path)
        # compressed BG graphics the patched graphics lists no longer decompress (rompatch GFXSKIP)
        gsk = sorted(set(names[a & 0x3FFFFF] for a in rompatch.gfxskip_sources(open(sys.argv[1], 'rb').read())
                         if (a & 0x3FFFFF) in names))
        print('mkgame: GFXSKIP drops %d compressed BG files' % len(gsk))
        drop = drop + gsk
        codep = set((a & 0x3FFFFF) >> 10 for a in insns)
        nd = 0
        for p in sorted(need):
            if p in codep or p in keep: continue
            j0 = bisect.bisect_right(starts, p << 10) - 1
            j1 = bisect.bisect_left(starts, (p + 1) << 10)
            ok = True
            if j1 > 0 and starts[j1 - 1] >= p << 10 and j1 - 1 >= max(j0, 0) and starts[j1 - 1] < (p + 1) << 10:
                ok = False                       # a block starts here: its header is read (sample scan)
            for j in range(max(j0, 0), j1):      # every block overlapping the page must be dropped data
                n = names.get(starts[j])
                if not n or not any(n.startswith(d) for d in drop): ok = False; break
            if ok: need.discard(p); nd += 1
        print('mkgame: dropped %d pages (%s)' % (nd, ','.join(drop)))
    need = sorted(p for p in need if p < 4096)
    blob = bytearray()
    idx = {}
    for p in need:
        idx[p] = len(blob) >> 10
        blob += rom[p << 10:(p + 1) << 10]
    open(os.path.join(out, 'snes_blob.bin'), 'wb').write(blob)
    open(os.path.join(out, 'snes_blob_host.bin'), 'wb').write(blob)   # (the host simulation: every page)
    pg_world = world_split(need, insns, starts, binpg, groups) if os.environ.get('WORLD_PROFILES') else None
    if pg_world is not None:
        # the 32X build: resident = the common pages only (the host simulation keeps all of them)
        com = [p for p in need if pg_world[p] & 0x80]
        open(os.path.join(out, 'snes_blob_host.bin'), 'wb').write(blob)
        cb = bytearray()
        for p in com: cb += rom[p << 10:(p + 1) << 10]
        open(os.path.join(out, 'snes_blob.bin'), 'wb').write(cb)
        import json
        nw = len(os.environ['WORLD_PROFILES'].split(','))
        wk = [int(it.split(':')[0]) for it in os.environ['WORLD_PROFILES'].split(',')]
        dset = set(DUP)          # (the DUP pages first: the same offset in every bank)
        json.dump(dict(common=com, worlds=wk, dup=DUP,
                       banks=[DUP + [p for p in need if pg_world[p] & (1 << i) and p not in dset] for i in range(nw)]),
                  open(os.path.join(out, 'worlds.json'), 'w'))
    # high WRAM pages resident (7E:2000-7FFF:FFFF in 256-byte pages; 0-1F are wram_lo)
    pool = [p for p in sorted(wpages) if p >= 0x20]
    scratch = []; ring = []
    clsf = os.path.join(os.path.dirname(prof), 'whcls.txt')
    if os.path.exists(clsf):
        cls = {}
        for ln in open(clsf):
            q = ln.split('#')[0].split()
            if len(q) == 2: cls[int(q[0], 16)] = int(q[1])
        pool = [p for p in sorted(cls) if p >= 0x20 and cls[p] & 3]
        scratch = [p for p in sorted(cls) if p >= 0x20 and not cls[p] & 3 and cls[p] & 8]
        ring = [p for p in sorted(cls) if p >= 0x20 and not cls[p] & 3 and cls[p] & 16]
    pidx = dict((p, i) for i, p in enumerate(pool))
    sidx = dict((p, i) for i, p in enumerate(scratch))
    ridx = dict((p, i) for i, p in enumerate(ring))
    c = ['/* generated by tools/recomp/mkgame.py - do not edit */', '#include <stdint.h>',
         'extern uint8_t wram_lo[0x2000];', 'extern uint8_t wram_dummy[256];',
         'extern const uint8_t snes_blob[];', 'const uint8_t zero_page[1024];',
         'uint8_t wram_pool[%d][256] __attribute__((aligned(16)));' % max(1, len(pool)),
         'const int game_npool = %d;' % len(pool),
         'extern uint32_t tiles_ram[];   /* load-only scratch pages alias the tile cache (sh/main.c) */',
         'const int game_nscratch = %d;' % len(scratch),
         'extern uint32_t ring_ent[];    /* class 16 pages alias the comp levels\' BG ring (sh/main.c) */',
         'const int game_nring = %d;' % len(ring),
         'uint8_t *const wram_hp[512] = {']
    ents = []
    for p in range(512):
        if p < 0x20: ents.append('wram_lo + 0x%X' % (p << 8))
        elif p in pidx: ents.append('wram_pool[%d]' % pidx[p])
        elif p in sidx: ents.append('(uint8_t *)tiles_ram + 0x%X' % (sidx[p] << 8))
        elif p in ridx: ents.append('(uint8_t *)ring_ent + 0x%X' % (ridx[p] << 8))
        else: ents.append('wram_dummy')
    for j in range(0, 512, 4): c.append('    ' + ', '.join(ents[j:j + 4]) + ',')
    c.append('};')
    # (SNES_PG_BANKED: the 32X table sits at the start of each world bank, cart window 6 -
    #  tools/worldbanks.py; this one = every page, for the host simulation)
    c.append('#ifndef SNES_PG_BANKED')
    c.append('const uint8_t *const snes_pg[4096] = {')
    ents = []
    for p in range(4096):
        ents.append('snes_blob + 0x%X' % (idx[p] << 10) if p in idx else 'zero_page')
    for j in range(0, 4096, 4): c.append('    ' + ', '.join(ents[j:j + 4]) + ',')
    c.append('};')
    c.append('#endif')
    if pg_world is not None:          # (host checks: bit 7 resident, bit k in world bank k)
        c.append('#ifdef R65_HOST_SIM')   # (I16: host checks only - 4 KB of resident ROM on the 32X)
        c.append('const uint8_t game_pg_world[4096] = {')
        for j in range(0, 4096, 32): c.append('    ' + ','.join('%d' % pg_world[p] for p in range(j, j + 32)) + ',')
        c.append('};')
        c.append('#endif')
    # snapshots (personal-use data): per entrance OUTDIR/snap_EE.bin = 8 x u32 registers, u32 LZ length,
    # LZ(WRAM lo + pool pages + SRAM), pad to 4, CGRAM 512 bytes - placed in the level's ROM bank by
    # build_viewer.py.  The OBJ VRAM (16 KB) is kept resident as tile indices into a shared pool of
    # unique tiles (the sprite code points at those tiles while the level runs).
    stab = []; pool_t = {}; pool_b = bytearray(); objtab = []; total = 0; rom_tiles = None
    pg_world_res = [q for q in need if pg_world[q] & 0x80 or q in DUP] if pg_world is not None else None
    for sp in snaps:
        path, ent = sp.rsplit(':', 1) if re.search(r':[0-9A-Fa-f]{1,3}$', sp) else (sp, '0')   # (C:\ paths; 100 = power-on)
        d = open(path, 'rb').read()
        rec = 48 + 0x20000 + 0x2000
        n = len(d) // rec
        r = d[(n - 1) * rec:n * rec]
        h = struct.unpack('<12I', r[:48])
        ram = r[48:48 + 0x20000]; sram = r[48 + 0x20000:48 + 0x20000 + 0x800]
        A, X, Y, S, D, DB, P, waipc = h[2], h[3], h[4], h[5], h[6], h[7], h[8], h[11]
        resume = ((P & 0x30) << 20) | ((waipc & 0xFF0000) | ((waipc + 1) & 0xFFFF))
        sb = bytearray(struct.pack('>8I', A, X, Y, S, D, DB, P, resume))
        raw = bytearray(ram[:0x2000])
        for p in pool: raw += ram[p << 8:(p + 1) << 8]
        for p in ring: raw += ram[p << 8:(p + 1) << 8]
        raw += sram
        z = lz_pack(bytes(raw))
        assert lz_unpack(z, len(raw)) == bytes(raw)
        sb += struct.pack('>I', len(z)) + z
        while len(sb) % 4: sb.append(0)
        base = path[:-4]
        vram = open(base + '.vram', 'rb').read() if os.path.exists(base + '.vram') else bytes(0x10000)
        cgram = open(base + '.cgram', 'rb').read() if os.path.exists(base + '.cgram') else bytes(512)
        sb += cgram[:512]
        open(os.path.join(out, 'snap_%02X.bin' % int(ent, 16)), 'wb').write(sb)
        total += len(sb)
        # (I16) the 512 OBJ tiles as runs: blank tiles (zero_page), tiles found verbatim in RESIDENT
        # ROM pages (ROM address: the runtime reads them through ROMB - they are DMA sources the game
        # used), the rest in the pool of unique tiles.  Was 512 pool indices per snapshot + a 46 KB
        # pool: ~89 KB of resident ROM; now ~25 KB (the room went to translating interpreted code)
        if rom_tiles is None:
            rom_tiles = {}
            res = set(pg_world_res) if pg_world_res is not None else set(need)
            for p in sorted(res):
                for o in range(0, 0x400, 2):
                    a = (p << 10) + o
                    if a + 32 > len(rom): break
                    if ((a + 31) >> 10) != p and ((a + 31) >> 10) not in res: continue
                    rom_tiles.setdefault(bytes(rom[a:a + 32]), a)
        runs = []
        for t in range(512):
            tl = bytes(vram[t * 32:t * 32 + 32])
            if tl == bytes(32): k = (0, 0)
            elif tl in rom_tiles: k = (1, rom_tiles[tl])
            else:
                if tl not in pool_t: pool_t[tl] = len(pool_t); pool_b += tl
                k = (2, pool_t[tl])
            if runs and runs[-1][0] == k[0] and runs[-1][2] < 512 and (
                    k[0] == 0 or (k[0] == 1 and k[1] == runs[-1][1] + 32 * runs[-1][2]) or (k[0] == 2 and k[1] == runs[-1][1] + runs[-1][2])):
                runs[-1][2] += 1
            else:
                runs.append([k[0], k[1], 1])
        stab.append(int(ent, 16)); objtab.append(runs)
    c.append('const uint8_t game_obj_pool[%d] __attribute__((aligned(4))) = {' % max(1, len(pool_b)))
    for j in range(0, len(pool_b), 32): c.append('    ' + ','.join('0x%02X' % v for v in pool_b[j:j + 32]) + ',')
    c.append('};')
    # runs: 3 x u16 = kind << 14 | (count - 1), operand >> 16, operand & 0xFFFF (kind 0 blank, 1 ROM
    # address, 2 pool tile index); game_snap_obj[k] = the index of snapshot k's first run
    allr = []; offs = []
    for runs in objtab:
        offs.append(len(allr) // 3)
        for kd, op, n in runs: allr += [kd << 14 | (n - 1), op >> 16, op & 0xFFFF]
    c.append('const uint16_t game_snap_runs[%d] = {' % max(1, len(allr)))
    for j in range(0, len(allr), 24): c.append('    ' + ','.join('%d' % v for v in allr[j:j + 24]) + ',')
    c.append('};')
    c.append('const uint16_t *const game_snap_obj[%d] = { %s };' % (max(1, len(offs)), ', '.join('game_snap_runs + %d' % (3 * o) for o in offs) or '0'))
    print('mkgame: snapshot OBJ tiles: %d runs, pool %d tiles (%d KB)' % (len(allr) // 3, len(pool_t), len(pool_b) // 1024))
    c.append('const uint16_t game_snap_ent[%d] = { %s0xFFFF };' % (len(stab) + 1, ''.join('0x%02X, ' % e for e in stab)))
    sblob = bytes(total)
    open(os.path.join(out, 'game_tab.c'), 'w').write('\n'.join(c) + '\n')
    print('mkgame: %d ROM pages (%d KB), %d WRAM pool pages, %d scratch pages, %d ring pages, %d snapshots (%d KB)' % (
        len(need), len(blob) // 1024, len(pool), len(scratch), len(ring), len(stab), len(sblob) // 1024))

if __name__ == '__main__':
    main()
