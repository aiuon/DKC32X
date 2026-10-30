#!/usr/bin/env python3
"""DKC 32X all-levels viewer: asset builder.

Reads the user's Donkey Kong Country (USA v1.0) ROM and tools/levels.json (recipes: names,
addresses and numbers only) and writes:
  build/gen/pack.bin     - data pack placed in the 32X ROM (read by both SH-2s and the 68000)
  build/gen/pack.h       - offsets / constants for the C code
  build/gen/preview/*.png - software composite of what each level should look like (debug)

Pack layout (all big-endian, items 4-byte aligned):
  'DKCP' u32 nlevels u32 nworlds u32 off_level_table u32 off_world_table
  level table: nlevels x u32 offset of LEVEL record
  world table: nworlds x u32 offset of WORLD record
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dkcrom import Rom, decode_4bpp_tile, decode_2bpp_tile  # noqa: E402
from vvram import Levels, build_vram, layout_tile, map_addr, map_words  # noqa: E402
import paths  # noqa: E402

GEN_LEVELS = [0, 29, 52, 70, 87, 101, 116, 130, 144, 158, 172, 187, 206, 228, 255]

# ------------------------------------------------------------------ per-type rendering config
# role of each SNES BG: 'play' (32X base, streamed from layout), 'ov' (32X overlay, static map),
# 'A'/'B' (Genesis plane), None (not shown in this milestone).
# scroll: (xmode, xshift, ymode, yshift, ysub, yclamp) ; mode 0: v>>shift, 1: v + v>>1, 2: v<<1,
# 3: constant (xshift used as value)
TYPES = {
    '808C6C': dict(name='jungle', play=1, layers={2: 'A', 3: 'B'}, scroll={2: (0, 1, 0, 2, 0, None), 3: (0, 3, 3, 0, 0, None)}, fx='jungle'),
    '808D43': dict(name='jungle-night', play=1, layers={2: 'A', 3: 'B'}, scroll={2: (0, 1, 0, 2, 0, None), 3: (0, 3, 3, 0, 0, None)}, fx='jungle'),
    '808CDC': dict(name='snow', play=1, layers={2: 'A', 3: 'B'}, scroll={2: (0, 1, 0, 3, 0, None), 3: (3, 0, 3, 0, 0, None)}, fx='snow'),
    '808DB4': dict(name='underwater', play=1, layers={2: 'A'}, scroll={2: (0, 1, 3, 0xFFFF, 0, None)}, fx='water'),
    '808E38': dict(name='cave', play=1, layers={2: 'A', 3: 'ov'}, scroll={2: (0, 1, 0, 3, 0, 0x11F), 3: (1, 0, 0, 0, 0, 0x11F)}, fx=None),
    '808F9C': dict(name='walkway', play=1, layers={2: 'A'}, scroll={2: (0, 1, 0, 3, 0, 0xF8)}, fx=None),
    '808FFE': dict(name='misty', play=1, layers={3: 'A'}, scroll={3: (0, 1, 0, 0, 0, None)}, fx=None),
    '80905C': dict(name='mc-madness', play=1, layers={2: 'A'}, scroll={2: (0, 1, 0, 3, 0, 0xF8)}, fx=None),
    '8090C3': dict(name='mc-carnage', play=1, layers={3: 'A'}, scroll={3: (0, 1, 0, 1, 0, None)}, fx=None),
    '809137': dict(name='mine', play=1, layers={2: 'A'}, scroll={2: (0, 1, 0, 1, 0, None)}, fx=None),
    '809196': dict(name='mine', play=1, layers={2: 'A'}, scroll={2: (0, 1, 0, 1, 0, None)}, fx=None),
    '8091F5': dict(name='factory', play=2, layers={1: 'ov', 3: 'A'}, scroll={1: (2, 0, 0, 0, 0, None), 3: (0, 1, 0, 3, 0, 0xF8)}, fx=None),
    '809446': dict(name='factory', play=2, layers={1: 'ov', 3: 'A'}, scroll={1: (2, 0, 0, 0, 0, None), 3: (0, 1, 0, 3, 0, 0xF8)}, fx=None),
    # temple: BG2 is the millstone sprite layer (positioned per millstone object), not scenery
    '809326': dict(name='temple', play=1, layers={3: 'B'}, scroll={3: (0, 1, 0, 3, 0, 0xF8)}, fx=None),
    '8093B5': dict(name='crystal', play=1, layers={2: 'A'}, scroll={2: (0, 1, 0, 1, 0, None)}, fx=None),
    # treehouse / forest: BG2/BG3 HOFS are written as 8 bits (CODE_8094CC / CODE_809543): xmode 4 wraps
    '8094CC': dict(name='treehouse', play=1, layers={2: 'A', 3: 'B'}, scroll={2: (4, 1, 0, 3, 0, 0xF8), 3: (4, 2, 3, 0, 0, None)}, fx=None),
    '809543': dict(name='forest', play=1, layers={2: 'A', 3: 'B'}, scroll={2: (4, 1, 0, 3, 0x180, None), 3: (4, 2, 3, 0, 0, None)}, fx=None),
    '8095B6': dict(name='boss', play=1, layers={2: 'A'}, scroll={2: (0, 1, 3, 0, 0, None)}, fx=None),
    # Kong's banana hoard (CODE_808F28): BG2 x = camera / 2, y fixed 4
    '808F28': dict(name='hoard', play=1, layers={2: 'A'}, scroll={2: (0, 1, 3, 4, 0, None)}, fx=None),
    # (I11) BG2 sky / sea: per-line perspective on the 68000 (fx 6, CODE_809A6E); BG3 2 x + $80 (CODE_809604)
    '809604': dict(name='galleon', play=1, layers={2: 'A', 3: 'B'}, scroll={2: (0, 2, 3, 0, 0, None), 3: (5, 0x80, 3, 0x10, 0, None)}, fx='galleon'),
}


def be16(v):
    return struct.pack('>H', v & 0xFFFF)


def be32(v):
    return struct.pack('>I', v & 0xFFFFFFFF)


def snes_rgb(w):
    return [((w >> s) & 31) for s in (0, 5, 10)]


def apply_colormath(c5, regs, layer):
    """SNES colour math against the fixed colour for one layer (no sub-screen layers)."""
    cg = regs['CGADSUB']
    if not (cg >> (layer - 1)) & 1 or (regs['CGWSEL'] & 2):
        return c5
    if regs['CGWSEL'] & 0x30:      # windowed math: skip in this milestone
        return c5
    f = regs['fixed']
    out = []
    for i in range(3):
        v = c5[i] - f[i] if cg & 0x80 else c5[i] + f[i]
        if cg & 0x40:
            v = v / 2.0
        out.append(max(0, min(31, v)))
    return out


# ------------------------------------------------------------------ playfield
class Tileset:
    """Tiles shared by all levels with the same layout base / char base."""

    def __init__(self, key):
        self.key = key
        self.pool = []          # 4bpp chunky tiles (32 bytes)
        self.pool_idx = {}      # (char base, bpp, tile) -> pool idx
        self.bake = []          # (pool idx, pal(0..15 in 16-colour units), hflip)
        self.bake_idx = {}
        self.vchunks = []       # 16 x (bake idx | vflip<<15)
        self.vprio = []         # per vchunk: bit (sy * 4 + sx) = SNES tile priority (BG1 high: covers OBJ priority 2)
        self.vchunk_idx = {}

    def pool_tile(self, vram, name, bpp, t):
        k = (name, bpp, t)
        if k not in self.pool_idx:
            tsz = 32 if bpp == 4 else 16
            off = (name * 2 + t * tsz) & 0xFFFF
            buf = bytes(vram[(off + i) & 0xFFFF] for i in range(tsz))
            px = decode_4bpp_tile(buf, 0) if bpp == 4 else decode_2bpp_tile(buf, 0)
            self.pool_idx[k] = len(self.pool)
            self.pool.append(bytes((px[i] << 4) | px[i + 1] for i in range(0, 64, 2)))
        return self.pool_idx[k]

    def bake_key(self, pidx, pal16, hflip):
        k = (pidx, pal16, hflip)
        if k not in self.bake_idx:
            self.bake_idx[k] = len(self.bake)
            self.bake.append(k)
        return self.bake_idx[k]

    def entry(self, vram, name, bpp, e, palbase=0):
        """SNES tilemap word -> bake idx | vflip<<15"""
        t, p, hf, vf = e & 0x3FF, (e >> 10) & 7, (e >> 14) & 1, (e >> 15) & 1
        pal16 = (palbase + p * (16 if bpp == 4 else 4)) // 4    # in units of 4 colours
        pidx = self.pool_tile(vram, name, bpp, t)
        return self.bake_key(pidx, pal16, hf) | (vf << 15)

    def composite(self, top, under):
        """(H17) bake idx of top (bake idx | vflip << 15) in front of under, one bake entry flagged 2:
        top bake << 19 | top vflip << 18 | under bake << 5 | under vflip << 4 | 2 (sh/main.c
        tc_composite); kept in self.bake as ('C', top, under)"""
        k = ('C', top, under)
        if k not in self.bake_idx:
            assert (top & 0x7FFF) < 8192 and (under & 0x7FFF) < 8192
            self.bake_idx[k] = len(self.bake)
            self.bake.append(k)
        return self.bake_idx[k]

    def plain(self, b):
        """bake tuple (pidx, pal16, hf) of bake idx b (a composite: its top tile)"""
        t = self.bake[b & 0x7FFF]
        return self.bake[t[1] & 0x7FFF] if t[0] == 'C' else t

    def bake_word(self, b):
        if b[0] == 'C':
            t, u = b[1], b[2]
            return (t & 0x7FFF) << 19 | (t >> 15) << 18 | (u & 0x7FFF) << 5 | (u >> 15) << 4 | 2
        pi, pal, hf = b
        return pi << 16 | pal << 8 | hf

    def vchunk(self, ent16, prio=0):
        k = (tuple(ent16), prio)
        if k not in self.vchunk_idx:
            self.vchunk_idx[k] = len(self.vchunks)
            self.vchunks.append(k[0])
            self.vprio.append(prio)
        return self.vchunk_idx[k]


def tile_is_empty(ts, e):
    b = ts.bake[e & 0x7FFF]
    if b[0] == 'C':
        return False
    return not any(ts.pool[b[0]])


def tile_is_full(ts, e):
    pidx = ts.bake[e & 0x7FFF][0]
    return all((b >> 4) and (b & 15) for b in ts.pool[pidx])


# ------------------------------------------------------------------ Genesis planes
def gen_level(c5, shadow):
    out = 0
    for i in range(3):
        target = c5[i] * 255.0 / 31.0
        best, bl = None, 0
        for L in range(8):
            lv = GEN_LEVELS[L] if shadow else GEN_LEVELS[L * 2]
            d = abs(lv - target)
            if best is None or d < best:
                best, bl = d, L
        out |= bl << (1 + i * 4)
    return out


# (I18) Bayer 4x4 with rows 2-3 rotated by one: both checkerboard halves get every other threshold
# (Bayer's own lower 8 all sit on one half) - the lamp cone's 50% dots cover one half per frame, and
# with plain Bayer the visible half was all 'below' pixels: dark tiles in the cone (BUGLOG I18)
BAYER4X4 = [[0, 8, 2, 10], [12, 4, 14, 6], [11, 1, 9, 3], [7, 13, 5, 15]]


def gen_level_dither(c5, shadow, x, y):
    """(I11) as gen_level, per channel the level below or above the SNES value by a 4x4 ordered
    dither: the dark scenes without S/H (the lamp levels) kept only the channels that rounded up -
    dark browns became black with red specks"""
    out = 0
    th = (BAYER4X4[y & 3][x & 3] + 0.5) / 16.0
    for i in range(3):
        target = c5[i] * 255.0 / 31.0
        lv = [GEN_LEVELS[L] if shadow else GEN_LEVELS[L * 2] for L in range(8)]
        lo = max(L for L in range(8) if lv[L] <= target)
        bl = lo
        if lo < 7:
            frac = (target - lv[lo]) / (lv[lo + 1] - lv[lo])
            if frac > th:
                bl = lo + 1
        out |= bl << (1 + i * 4)
    return out


def _planes_clustered(info, anim):
    """Genesis planes without shadow/highlight, palette lines by clustering: each tile goes to the line
    that reproduces it best, each line's 15 colours are a weighted k-means of its tiles' pixels
    (snapped to the Genesis colour levels).  Better than first-fit for colour-rich layers."""
    import numpy as np
    keys, pix, wts = [], [], []
    for pi in info:
        cnt = {}
        for row in pi['ents']:
            for e in row:
                k = (e & 0x3FF, (e >> 10) & 7)
                cnt[k] = cnt.get(k, 0) + 1
        for k, (px, cmap) in pi['cols'].items():
            if not cmap:
                continue
            rgb = [[c * 255.0 / 31 for c in cmap[v]] if v else None for v in px]
            keys.append((pi['letter'], k))
            pix.append(rgb)
            wts.append(cnt.get(k, 1))
    n = len(keys)
    # per tile: opaque pixel colours (with multiplicity)
    tcols = [np.array([c for c in rgb if c is not None]) for rgb in pix]
    # dark scenes: shadow mode (all cells low priority) has the finer dark levels 0, 29, 52, 70, ...
    allc = np.concatenate([tc for tc in tcols])
    bright = (allc.max(1) > 140).mean()
    shadow = bright < 0.04
    GL = np.array([GEN_LEVELS[L] if shadow else GEN_LEVELS[L * 2] for L in range(8)], float)
    print('   clustered planes: %.1f%% bright pixels -> %s' % (100 * bright, 'shadow' if shadow else 'normal'))
    rng = np.random.RandomState(1)
    # init: 4 groups by mean colour (k-means on tile means)
    means = np.array([tc.mean(0) for tc in tcols])
    cent = means[rng.choice(n, 4, replace=False)]
    for _ in range(10):
        a = ((means[:, None, :] - cent[None]) ** 2).sum(2).argmin(1)
        for l in range(4):
            if (a == l).any():
                cent[l] = means[a == l].mean(0)
    assign = a
    pals = [None] * 4

    def snap(c):
        return GL[np.abs(GL[None, :] - c[:, None]).argmin(1)] if c.ndim == 1 else \
            np.stack([GL[np.abs(GL[None, :] - c[:, i:i + 1]).argmin(1)] for i in range(3)], 1)
    for it in range(8):
        for l in range(4):
            idx = np.nonzero(assign == l)[0]
            if not len(idx):
                pals[l] = np.zeros((1, 3))
                continue
            pts = np.concatenate([tcols[i] for i in idx])
            w = np.concatenate([np.full(len(tcols[i]), wts[i], float) for i in idx])
            uniq, inv = np.unique(np.round(pts), axis=0, return_inverse=True)
            uw = np.bincount(inv.ravel(), weights=w)
            k = min(15, len(uniq))
            c = uniq[np.argsort(-uw)[:k]].copy()
            for _ in range(12):
                d = ((uniq[:, None, :] - c[None]) ** 2).sum(2).argmin(1)
                for j in range(k):
                    m = d == j
                    if m.any():
                        c[j] = (uniq[m] * uw[m, None]).sum(0) / uw[m].sum()
            pals[l] = np.unique(snap(c), axis=0)
        if it == 7:
            break
        err = np.zeros((n, 4))
        for l in range(4):
            for i in range(n):
                err[i, l] = ((tcols[i][:, None, :] - pals[l][None]) ** 2).sum(2).min(1).sum()
        assign = err.argmin(1)
    # emit
    cram = [0] * 64
    for l in range(4):
        for j, c in enumerate(pals[l][:15]):
            lv3 = [int(np.abs(GL - c[i]).argmin()) for i in range(3)]
            cram[l * 16 + 1 + j] = lv3[0] << 1 | lv3[1] << 5 | lv3[2] << 9
    tiles = [bytes(32)]
    tile_idx = {}
    word = {}
    for i, key in enumerate(keys):
        l = int(assign[i])
        P = pals[l][:15]
        idx = []
        for c in pix[i]:
            if c is None:
                idx.append(0)
            else:
                idx.append(1 + int(((P - np.array(c)) ** 2).sum(1).argmin()))
        tb = bytes((idx[j] << 4) | idx[j + 1] for j in range(0, 64, 2))
        if (tb, l) not in tile_idx:
            tile_idx[(tb, l)] = len(tiles)
            tiles.append(tb)
        word[key] = tile_idx[(tb, l)] | (l << 13) | (0 if shadow else 0x8000)
    maps = {}
    for pi in info:
        m = []
        for row in pi['ents']:
            for e in row:
                k = (pi['letter'], (e & 0x3FF, (e >> 10) & 7))
                v = word.get(k, 0)
                if v:
                    if e & 0x4000:
                        v |= 0x0800
                    if e & 0x8000:
                        v |= 0x1000
                m.append(v)
        gw = 64
        gh = 64 if pi['hgt'] == 64 else 32
        out = [m[(ty % pi['hgt']) * pi['wid'] + (tx % pi['wid'])] for ty in range(gh) for tx in range(gw)]
        maps[pi['letter']] = dict(map=out, w=gw, h=gh, shadow=shadow, layer=pi['layer'], raw=m, rawwid=pi['wid'])
    return tiles, maps, cram, []


def genesis_planes(vram, cg, lv, planes, dyn_layer=None, anim=None, ents_override=None, noshadow=False, day=None,
                   normal=False):
    """planes: list of (plane letter, snes layer).  Returns tiles, maps, cram, anim slots.
    anim = (layer, first snes tile, count, [frame bytes] [, group split]): those tiles change every
    frame pair; returns [(genesis tile (| 0x8000 = second group), [frame tiles])] for the 68000.
    day = {'cgs': [palettes]} (or {'cg': palette}): palette keyframes for weather fades - colours are
    told apart by their values in every keyframe, and day['crams'] gets one CRAM per keyframe."""
    regs = lv['regs']
    tiles = [bytes(32)]
    tile_idx = {}
    info = []
    day_cgs = (day['cgs'] if 'cgs' in day else [day['cg']]) if day is not None else []
    all_cgs = [cg] + list(day_cgs)
    specs = ([] if not anim else [anim] if isinstance(anim, tuple) else list(anim))

    def anim_spec(t, layer):
        for i, a in enumerate(specs):
            if layer == a[0] and a[1] <= t < a[1] + a[2]:
                return i
        return None

    def anim_tile(t, layer):
        return anim_spec(t, layer) is not None

    def anim_px(t, f, layer):
        a = specs[anim_spec(t, layer)]
        buf = a[3][f][(t - a[1]) * 32:(t - a[1]) * 32 + 32]
        return decode_4bpp_tile(buf, 0)
    # used colour indices per (layer, snes tile, pal)
    for letter, layer in planes:
        bg = lv['bgs'][layer - 1]
        bpp = bg['bpp']
        wid = 64 if bg['scs'] & 1 else 32
        hgt = 64 if bg['scs'] & 2 else 32
        if ents_override and letter in ents_override:
            ents = ents_override[letter]
            wid, hgt = len(ents[0]), len(ents)
        else:
            ents = [[vram[map_addr(bg['sc'], bg['scs'], tx, ty) * 2] | vram[map_addr(bg['sc'], bg['scs'], tx, ty) * 2 + 1] << 8
                     for tx in range(wid)] for ty in range(hgt)]
        maxc = 0
        cols = {}
        for row in ents:
            for e in row:
                t, p = e & 0x3FF, (e >> 10) & 7
                if (t, p) in cols:
                    continue
                tsz = 32 if bpp == 4 else 16
                off = (bg['name'] * 2 + t * tsz) & 0xFFFF
                buf = bytes(vram[(off + i) & 0xFFFF] for i in range(tsz))
                px = decode_4bpp_tile(buf, 0) if bpp == 4 else decode_2bpp_tile(buf, 0)
                vs = set(v for v in px if v)
                if anim_tile(t, layer):              # the animation frames' colours too
                    for f in range(len(specs[anim_spec(t, layer)][3])):
                        vs |= set(v for v in anim_px(t, f, layer) if v)
                base = p * (16 if bpp == 4 else 4)
                for v in vs:
                    if not (layer == dyn_layer and v == 1):
                        w = cg[(base + v) * 2] | cg[(base + v) * 2 + 1] << 8
                        maxc = max(maxc, max(apply_colormath(snes_rgb(w), regs, layer)))
                cols[(t, p)] = (px, vs, base)
        info.append(dict(letter=letter, layer=layer, ents=ents, cols=cols, wid=wid, hgt=hgt, maxc=maxc))
    if noshadow:
        for pi in info:                          # (clustered path: colour maps as before)
            pi['cols'] = {k: (px, {v: apply_colormath(snes_rgb(cg[(base + v) * 2] | cg[(base + v) * 2 + 1] << 8),
                                                      regs, pi['layer']) for v in vs})
                          for k, (px, vs, base) in pi['cols'].items()}
        return _planes_clustered(info, anim)
    # shadow decisions: plane A over plane B
    for pi in info:
        pi['shadow'] = pi['maxc'] <= 15.5 and not noshadow and not normal and pi['letter'] in ('A', 'B')
        if normal:
            # (I18) lamp levels: S/H on, the dark wall shadowed (the finer dark levels 0, 29, 52, 70 ..
            # instead of the I11 ordered dither between 0 and 52 - a dot pattern all over the wall that
            # the cone's checkerboard lined up with); cells with colours brighter than the shadow
            # levels reach (the lanterns' flames) stay normal: high priority, in front of the cone
            pi['shadow'] = pi['letter'] in ('A', 'B')
            pi['bright'] = set(k for k, (px, vs, base) in pi['cols'].items()
                               if any(max(apply_colormath(snes_rgb(cg[(base + v) * 2] | cg[(base + v) * 2 + 1] << 8),
                                                          regs, pi['layer'])) > LAMP_BRIGHT5 for v in vs))
    if any(pi['letter'] == 'S' for pi in info):  # weather snow layer: S/H stays off
        for pi in info:
            pi['shadow'] = False
    a = [p for p in info if p['letter'] == 'A']
    b = [p for p in info if p['letter'] == 'B']
    if a and b and a[0]['shadow'] and not b[0]['shadow']:
        a[0]['shadow'] = False
    # Genesis S/H: a pixel is shadowed only when no plane has a high-priority tile there - even a
    # transparent one.  A non-shadowed (priority) plane A would un-shadow plane B in whole cells.
    if a and b and not a[0]['shadow'] and b[0]['shadow']:
        b[0]['shadow'] = False

    def tsh(pi, k):
        """shadowed cell (low priority under S/H)"""
        return pi['shadow'] and k not in pi.get('bright', ())

    def tdither(pi, k):
        """lamp levels: 4x4 ordered dither (I11) - in the shadowed cells between the fine shadow levels
        (0 / 29 / 52 ..: a faint pattern, the wall's browns on average), in the lantern cells between
        the normal levels (their glow next to the shadowed wall)"""
        return normal and day is None

    def dkey(pi, base, v, i, sh):
        """(dither) the Genesis colour of SNES colour base + v at tile pixel i"""
        if pi['layer'] == dyn_layer and v == 1:
            return 'DYN'
        return gen_level_dither(apply_colormath(snes_rgb(cg[(base + v) * 2] | cg[(base + v) * 2 + 1] << 8), regs, pi['layer']),
                                sh, i & 7, i >> 3)

    def colkey(pi, base, v, sh):
        """Genesis colour of SNES colour base + v (a tuple over the keyframes for weather levels)."""
        if pi['layer'] == dyn_layer and v == 1:
            return 'DYN'
        ks = tuple(gen_level(apply_colormath(snes_rgb(q[(base + v) * 2] | q[(base + v) * 2 + 1] << 8), regs, pi['layer']),
                             sh) for q in all_cgs)
        return ks if day is not None else ks[0]
    # palette lines: line 0 colours 1..15 shared, lines 1..3
    canon = {}

    def ckey(pi, base, v, sh):
        c = colkey(pi, base, v, sh)
        while c in canon:
            c = canon[c]
        return c

    def allocate():
        lines = [dict() for _ in range(4)]
        if dyn_layer:
            lines[0]['DYN'] = 1
        for i, k in enumerate((day or {}).get('reserve3', [])):   # fixed colours at line 3, 1..n
            lines[3][k] = i + 1
        sets = []
        for pi in info:
            for k, (px, vs, base) in pi['cols'].items():
                if tdither(pi, k):
                    sets.append((pi['letter'], k, frozenset(dkey(pi, base, v, i, tsh(pi, k)) for i, v in enumerate(px) if v)))
                else:
                    sets.append((pi['letter'], k, frozenset(ckey(pi, base, v, tsh(pi, k)) for v in vs)))
        assign = {}
        if day is not None:
            # weather levels (many keyframe-distinct colours): place each SNES palette's whole colour
            # set first, so the tiles of one palette share a line instead of fragmenting the lines
            pun = {}
            for letter, k, s in sets:
                pun.setdefault((letter, k[1]), set()).update(s)
            for s in sorted(pun.values(), key=lambda x: (-len(x), sorted(map(str, x)))):
                best = None
                for li in range(4):
                    have = set(lines[li].keys())
                    grow = len(s - have)
                    if len(have) + grow <= 15 and (best is None or grow < best[0]):
                        best = (grow, li)
                if best is not None:
                    for c in sorted(s - set(lines[best[1]].keys()), key=str):
                        lines[best[1]][c] = len(lines[best[1]]) + 1
        misfit = 0
        nl = 3 if normal else 4                   # (lamp levels: line 3 = the cones' colours)
        for letter, k, s in sorted(sets, key=lambda x: -len(x[2])):
            best = None
            for li in ([0] if 'DYN' in s else range(nl)):
                have = set(lines[li].keys())
                grow = len(s - have)
                if len(have) + grow <= 15 and (best is None or grow < best[0]):
                    best = (grow, li)
            if best is None:
                best = (0, max(range(nl), key=lambda li: len(s & set(lines[li].keys()))))
                misfit += 1
            else:
                for c in s:
                    if c not in lines[best[1]]:
                        lines[best[1]][c] = len(lines[best[1]]) + 1
            assign[(letter, k)] = best[1]
        return lines, assign, misfit, sets

    def kdist(x, y):
        return sum((((p >> (1 + i * 4)) & 7) - ((q >> (1 + i * 4)) & 7)) ** 2 for p, q in zip(x, y) for i in range(3))
    lines, assign, misfit, sets = allocate()
    merged = 0
    while day is not None and misfit and merged < 40:
        # merge the two closest colours (over all keyframes) and pack again
        used = sorted(set(c for _, _, s in sets for c in s if c != 'DYN'))
        x, y = min(((p, q) for i, p in enumerate(used) for q in used[i + 1:]), key=lambda pq: kdist(*pq))
        canon[y] = x
        merged += 1
        lines, assign, misfit, sets = allocate()
    if merged:
        print('   merged %d near-identical colours to fit the palette lines' % merged)
    nk = len(all_cgs)
    crams = [[0] * 64 for _ in range(nk)]
    for li in range(4):
        for c, idx in lines[li].items():
            for q in range(nk):
                crams[q][li * 16 + idx] = 0 if c == 'DYN' else c[q] if day is not None else c
    cram = crams[0]
    if day is not None:
        day['crams'] = crams[1:]
        day['cram'] = crams[1]

    def nearest(li, c):
        def rgb(v):
            vv = v if isinstance(v, tuple) else (v,)
            return [0] * 3 * len(vv) if v == 'DYN' else [(x >> (1 + i * 4)) & 7 for x in vv for i in range(3)]
        cr = rgb(c)
        best = min(lines[li].items(), key=lambda kv: sum((x - y) ** 2 for x, y in zip(rgb(kv[0]), cr)))
        return best[1]
    # (I19) palette-constrained ordered dither: a SNES colour the Genesis cannot show is drawn as a
    # 4x4 ordered mix of two colours ALREADY in the tile's palette line (no new colours, no new tiles:
    # the pattern is fixed per tile pixel).  The matrix gives both checkerboard halves every other
    # threshold, so a 50 % checkerboard over it (fog, lamp dots) never lines up with it, at any scroll.
    pdither = PLANE_DITHER and day is None and not normal
    pd_cache = {}

    def pd_index(li, pi, base, v, i, sh):
        if pi['layer'] == dyn_layer and v == 1:
            return None
        c5 = apply_colormath(snes_rgb(cg[(base + v) * 2] | cg[(base + v) * 2 + 1] << 8), regs, pi['layer'])
        T = tuple(min(31, x) * 255.0 / 31 for x in c5)
        ck = (li, T, sh)
        if ck not in pd_cache:
            ents = [(ix, cgen(kv, sh)) for kv, ix in lines[li].items() if kv != 'DYN']
            W = (3.0, 4.0, 2.0)

            def d2(a, b):
                return sum(w * (x - y) ** 2 for w, x, y in zip(W, a, b))
            ia, ca = min(ents, key=lambda e: d2(e[1], T))
            e1 = d2(ca, T)
            best = None
            if e1 > PD_MIN_ERR:
                for j, (ix, cx) in enumerate(ents):
                    for iy, cy in ents[j + 1:]:
                        dd = [y - x for x, y in zip(cx, cy)]
                        n2 = sum(w * q * q for w, q in zip(W, dd))
                        if not n2:
                            continue
                        r = sum(w * q * (t - x) for w, q, t, x in zip(W, dd, T, cx)) / n2
                        r16 = int(round(r * 16))
                        if r16 < 1 or r16 > 15:
                            continue
                        mix = [x + q * r16 / 16.0 for x, q in zip(cx, dd)]
                        err = d2(mix, T) + PD_SPREAD * n2
                        if best is None or err < best[0]:
                            best = (err, ix, iy, r16)
            if best is not None and best[0] < e1 * PD_GAIN:
                pd_cache[ck] = (best[1], best[2], best[3])
            else:
                pd_cache[ck] = (ia, ia, 0)
        a, b, r16 = pd_cache[ck]
        return b if BAYER4X4[(i >> 3) & 3][i & 3] < r16 else a
    maps = {}
    anim_keys = set()
    for pi in info:
        m = []
        for row in pi['ents']:
            for e in row:
                t, p = e & 0x3FF, (e >> 10) & 7
                k = (t, p)
                li = assign[(pi['letter'], k)]
                px, vs, base = pi['cols'][k]
                is_anim = anim_tile(t, pi['layer'])
                if not vs and not is_anim:
                    m.append(0)
                    continue
                key = (pi['letter'], k, li)
                if is_anim:
                    anim_keys.add((key, pi['layer']))
                if key not in tile_idx:
                    idx = []
                    for i, v in enumerate(px):
                        if not v:
                            idx.append(0)
                        else:
                            q = pd_index(li, pi, base, v, i, tsh(pi, k)) if pdither else None
                            if q is not None:
                                idx.append(q)
                                continue
                            g = dkey(pi, base, v, i, tsh(pi, k)) if tdither(pi, k) else ckey(pi, base, v, tsh(pi, k))
                            idx.append(lines[li].get(g) or nearest(li, g))
                    tile_idx[key] = len(tiles)
                    tiles.append(bytes((idx[i] << 4) | idx[i + 1] for i in range(0, 64, 2)))
                v = tile_idx[key] | (li << 13)
                if e & 0x4000:
                    v |= 0x0800
                if e & 0x8000:
                    v |= 0x1000
                if not tsh(pi, k):
                    v |= 0x8000
                m.append(v)
        # expand to Genesis plane: width 64 (repeat 32-wide maps), height 32 or 64
        gw = 64
        gh = 64 if pi['hgt'] == 64 else 32
        out = []
        for ty in range(gh):
            for tx in range(gw):
                out.append(m[(ty % pi['hgt']) * pi['wid'] + (tx % pi['wid'])])
        maps[pi['letter']] = dict(map=out, w=gw, h=gh, shadow=pi['shadow'], layer=pi['layer'], raw=m, rawwid=pi['wid'])
    if day is not None:
        print('   keyframe cram: %d colours x %d keyframes, %d tiles without a full line, shadow %s' % (
            sum(len(l) for l in lines), nk - 1, misfit, [pi['shadow'] for pi in info]))
    slots = []
    for key, layer in sorted(anim_keys):
        letter, (t, p), li = key
        pi = [q for q in info if q['letter'] == letter][0]
        sp = specs[anim_spec(t, layer)]
        if True:
            base = p * 16
            frames = []
            for f in range(len(sp[3])):
                idx = []
                for v in anim_px(t, f, layer):
                    if not v:
                        idx.append(0)
                        continue
                    q = pd_index(li, pi, base, v, len(idx), tsh(pi, (t, p))) if pdither else None
                    if q is not None:
                        idx.append(q)
                        continue
                    g = ckey(pi, base, v, tsh(pi, (t, p)))
                    idx.append(lines[li].get(g) or nearest(li, g))
                frames.append(bytes((idx[i] << 4) | idx[i + 1] for i in range(0, 64, 2)))
            grp = 0x8000 if (len(sp) > 4 and t - sp[1] >= sp[4]) or (len(specs) > 1 and sp is specs[1]) else 0
            slots.append((tile_idx[key] | grp, frames))
    return tiles, maps, cram, slots


# ------------------------------------------------------------------ level build
def sky_table(L, lv, cg):
    """224 backdrop colours (SNES CGRAM 0 per line)."""
    if lv['nmi'] in ('808C6C', '808D43'):
        rom = L.rom
        data = rom.read(0xB996E6, 0x220)
        out = []
        i = 0
        while i < len(data) and len(out) < 224:
            n = data[i]
            if n == 0:
                break
            out += [data[i + 3] | data[i + 4] << 8] * (n & 0x7F)
            i += 5
        while len(out) < 224:
            out.append(out[-1])
        return out[:224]
    c0 = cg[0] | cg[1] << 8
    return [c0] * 224


# ------------------------------------------------------------------ runtime palettes
# Some levels build their palette at runtime with the weather/fade engine (CODE_80AC54 / CODE_80AF53
# pre-runs 256 fade frames, so the first visible palette is the fade *target*).  Reproduced here.
def _ch(c):
    return c & 31, (c >> 5) & 31, (c >> 10) & 31


def survey_cgram(lv, cg):
    """Colours the level's CGRAM recipe does not cover (the game computes them at run time - e.g. Stop
    & Go's BG2 cave wall = palette 1, set by the light code) from the snes9x capture of YOUR ROM made
    at the level start (tools/capture.py: DKC_SURVEY/EE/cgram.bin).  runtime_palette() may override."""
    p = os.path.join(paths.SURVEY, '%02X' % lv['entrance'], 'cgram.bin')
    unc = lv.get('uncovered_cgram') or []
    if not unc or not os.path.exists(p):
        return cg
    sv = open(p, 'rb').read()
    out = bytearray(cg)
    for i in unc:
        if i < 256:
            out[i * 2:i * 2 + 2] = sv[i * 2:i * 2 + 2]
    return bytes(out)


def runtime_palette(L, lv, cg):
    """Returns (cg bytes, sky override or None)."""
    e = lv['entrance']
    rom = L.rom
    pal = [cg[i * 2] | cg[i * 2 + 1] << 8 for i in range(256)]
    sky = None
    if e in (0x0C, 0x1C, 0x1B):
        # Ropey Rampage storm (and its bonus rooms) (CODE_80AC8B): colours 16..127 of the jungle palette fade to
        # r*5/16, g*5/8 and blue = that green (8.8 / 5.5 fixed point like the game).
        for i in range(16, 128):
            R, G, B = _ch(rom.u16(0xB9A1DC + i * 2))
            r2 = ((R << 8) >> 2) + ((R << 8) >> 4)
            g = G << 5
            g2 = ((g >> 1) + (g >> 3)) & 0x3E0
            pal[i] = (r2 >> 8) | g2 | (g2 << 5)
        # sky: lines 0..70 fade to DATA_B99658, the rest to black
        sky = [rom.u16(0xB99658 + i * 2) for i in range(71)] + [0] * (224 - 71)
    elif e == 0xA7:
        # Ice Age Alley blizzard (CODE_80AA42): colours 0-31 halved, 32-127 halved + 5,
        # BG3 snow colours 1-3 from DATA_B99B14.
        for i in range(128):
            R, G, B = _ch(rom.u16(0xB99B1C + i * 2))
            if i < 32:
                pal[i] = (R >> 1) | (G >> 1) << 5 | (B >> 1) << 10
            else:
                pal[i] = ((R >> 1) + 5) | ((G >> 1) + 5) << 5 | ((B >> 1) + 5) << 10
        for i in range(4):
            pal[i] = rom.u16(0xB99B14 + i * 2)
    elif lv['nmi'] in ('808F9C', '80905C'):
        # walkway levels: BG2 (cave wall) colours 17-31 = DATA_B9A2FC dimmed by the flickering lamp
        # level (CODE_80B800: -1..-4 per component); use the brightest (-1): Genesis has 3 bits per channel
        for i in range(1, 16):
            R, G, B = _ch(rom.u16(0xB9A2FC + i * 2))
            pal[16 + i] = max(0, R - 1) | max(0, G - 1) << 5 | max(0, B - 1) << 10
        # colours 1-15 are the lamp light, set per line by HDMA near lamps (CODE_80B2xx): dark elsewhere;
        # colour 0 (backdrop) is written black every frame (CODE_80B1BA)
        for i in range(0, 16):
            pal[i] = 0
    elif lv['nmi'] in ('809137', '809196'):
        # Stop & Go / Loopy Lights (barrel lights): BG2 (the cave wall, palette 1) = DATA_B9A2FC like
        # the walkway levels; the light keyframes (LIGHTS) darken / brighten it.  (The capture holds the
        # per-line lamp HDMA's last values in palette 1 - not the wall.)
        for i in range(1, 16):
            pal[16 + i] = rom.u16(0xB9A2FC + i * 2)
    else:
        return cg, None
    out = bytearray(cg)
    for i in range(128):
        out[i * 2] = pal[i] & 0xFF
        out[i * 2 + 1] = pal[i] >> 8
    return bytes(out), sky


# ------------------------------------------------------------------ heavy snow (weather level 5)
# CODE_8089A0 with $1DF1 == 5: BG3 map $7C00 (TM_Layer3_Snow), chars at $6000 re-streamed every
# odd frame from DATA_DECE74 + 0x400 * ((frame & 0xE) >> 1); BG3 is high priority (in front of BG1),
# so on the 32X it is an animated overlay.  HOFS = (frame + sway) & 0xFF (sway from DATA_808BA8),
# per-line VOFS wobble from a 32-line wave (HDMA ch2, table start moves 1 line every 2 frames).
SNOW_FRAMES = 8
SNOW_TILES = 64
SNOW_WOBBLE = [0, 0, 0, 1, 1, 1, 2, 2, 2, 2, 2, 1, 1, 1, 0, 0,
               0, 0, 0, -1, -1, -1, -2, -2, -2, -2, -2, -1, -1, -1, 0, 0]


def heavy_snow_overlay(L, lv, vram, ts, sc=None):
    rom = L.rom
    bg = dict(lv['bgs'][2])
    if sc is not None:
        bg['sc'], bg['scs'] = sc, 0
    m = []
    for ty in range(32):
        for tx in range(32):
            a = map_addr(bg['sc'], bg['scs'], tx, ty) * 2
            e = vram[a] | vram[a + 1] << 8
            m.append(0x4000 | (e & 0x3F) | (e & 0x8000))
    bakes = []
    for f in range(SNOW_FRAMES):
        data = rom.read(0xDECE74 + 0x400 * f, 0x400)
        for t in range(SNOW_TILES):
            k = ('snow', f, t)
            if k not in ts.pool_idx:
                px = decode_2bpp_tile(data, t * 16)
                ts.pool_idx[k] = len(ts.pool)
                ts.pool.append(bytes((px[i] << 4) | px[i + 1] for i in range(0, 64, 2)))
            bakes.append(ts.bake_key(ts.pool_idx[k], 0, 0))
    sway = list(rom.read(0x808BA8, 64))
    # per frame, per overlay row: list of (x, colour) flake pixels (the SH-2 only writes these)
    flakes = []
    for f in range(SNOW_FRAMES):
        data = rom.read(0xDECE74 + 0x400 * f, 0x400)
        tiles = [decode_2bpp_tile(data, t * 16) for t in range(SNOW_TILES)]
        rows = []
        for oy in range(256):
            row = []
            for tx in range(32):
                e = m[(oy >> 3) * 32 + tx]
                ty = oy & 7
                if e & 0x8000:
                    ty = 7 - ty
                px = tiles[e & 0x3F]
                for x in range(8):
                    c = px[ty * 8 + x]
                    if c:
                        row.append((tx * 8 + x, c))
            rows.append(row)
        flakes.append(rows)
    return dict(layer=3, w=32, h=32, map=m, scroll=(4, 0, 4, 0, SNOW_TILES, SNOW_FRAMES),
                anim=bakes, sway=sway, wobble=SNOW_WOBBLE, flakes=flakes)


# ------------------------------------------------------------------ comp mode (true transparency)
# Levels whose colour math mixes layers that would otherwise be split between the 32X and the Genesis.
# Everything goes on the 32X: base layer (static map) + BG1 playfield + light layer.  The scene is
# quantized to S colours, the light to K levels, and palette bank k holds blend(light_k, scene) so the
# SH-2 lights a pixel by adding k*S to its byte.
COMP = {
    # S * K banks + an unlit bank of S colours at U = K * S (performance mode: BG1 drawn unfogged)
    0x22: dict(base=2, light=3, op='addhalf', S=36, K=6, perf=True,   # Poison Pond: (fog + scene) / 2
               lscroll=(1, 0, 0, 0, 0, None)),                  # BG3: x*1.5, y = camera y (CODE_808DB4)
    # Misty Mine: BG2 fog on main, scene (BG1 + BG3) on sub, CGADSUB $22 add.  Fog colour v (0-15) is
    # grey floor(v * I / 32) with intensity I fading 0 -> 31 (CODE_80B4CA); levels = v pairs, palette
    # recomputed at run time.  BG2 H/V scroll per 4-line band from a wave table (CODE_80B547).
    0x0A: dict(base=3, light=2, op='add', S=28, K=8, lgroup='vpair', wave='misty', perf=True),
}


def _kmeans(points, k, iters=30):
    import random
    pts = list(points)
    if len(pts) <= k:
        return pts, list(range(len(pts)))
    random.seed(1)
    cents = [pts[i] for i in sorted(random.sample(range(len(pts)), k))]
    assign = [0] * len(pts)
    for _ in range(iters):
        for i, q in enumerate(pts):
            assign[i] = min(range(len(cents)), key=lambda c: sum((a - b) ** 2 for a, b in zip(q, cents[c])))
        for c in range(len(cents)):
            mem = [pts[i] for i in range(len(pts)) if assign[i] == c]
            if mem:
                cents[c] = tuple(sum(m[j] for m in mem) / len(mem) for j in range(len(q)))
    return cents, assign


def comp_level(L, lv, vram, cg, ts, t, grid_bakes):
    cfg = COMP[lv['entrance']]
    S, K = cfg['S'], cfg['K']
    col = lambda i: cg[i * 2] | cg[i * 2 + 1] << 8
    rgb = lambda w: (w & 31, (w >> 5) & 31, (w >> 10) & 31)
    # ---- base layer map (static), as an overlay-style map of bake entries
    bg = lv['bgs'][cfg['base'] - 1]
    wid = 64 if bg['scs'] & 1 else 32
    hgt = 64 if bg['scs'] & 2 else 32
    base_map = []
    for ty in range(hgt):
        for tx in range(wid):
            a = map_addr(bg['sc'], bg['scs'], tx, ty) * 2
            base_map.append(ts.entry(vram, bg['name'], bg['bpp'], vram[a] | vram[a + 1] << 8))
    # ---- scene colours actually used (CGRAM indices)
    used = set()
    for be in list(grid_bakes) + [e & 0x7FFF for e in base_map]:
        pidx, pal16, hf = ts.plain(be)
        for byte in ts.pool[pidx]:
            for v in (byte >> 4, byte & 15):
                if v:
                    used.add(pal16 * 4 + v)
    ucols = sorted(set(col(i) for i in used))
    cents, assign = _kmeans([rgb(c) for c in ucols], S - 1)
    qcol = [rgb(col(0))] + [tuple(int(round(x)) for x in c) for c in cents]
    qof = {c: 1 + assign[i] for i, c in enumerate(ucols)}
    remap = [1] * 256
    remap[0] = 0
    for i in used:
        remap[i] = qof[col(i)]
    # ---- light layer: colours per pixel value, quantized to K-1 levels
    lb = lv['bgs'][cfg['light'] - 1]
    lw = 64 if lb['scs'] & 1 else 32
    lh = 64 if lb['scs'] & 2 else 32
    lents = []
    for ty in range(lh):
        for tx in range(lw):
            a = map_addr(lb['sc'], lb['scs'], tx, ty) * 2
            lents.append(vram[a] | vram[a + 1] << 8)
    lbpp = lb['bpp']
    vpair = cfg.get('lgroup') == 'vpair'
    vals = set()
    for e in lents:
        tsz = 16 if lbpp == 2 else 32
        off = (lb['name'] * 2 + (e & 0x3FF) * tsz) & 0xFFFF
        px = (decode_2bpp_tile if lbpp == 2 else decode_4bpp_tile)(bytes(vram[(off + i) & 0xFFFF] for i in range(tsz)), 0)
        p = (e >> 10) & 7
        for v in set(px):
            if v:
                vals.add(p * (4 if lbpp == 2 else 16) + v)
    lcols = sorted(set(col(v) for v in vals))
    if vpair:
        # light level = pixel value pair (v >> 1); the greys depend on the run-time intensity
        levels = [(0, 0, 0)] * K
    else:
        lcent, lassign = _kmeans([rgb(c) for c in lcols], K - 1)
        lvl_of_col = {c: 1 + lassign[i] for i, c in enumerate(lcols)}
        levels = [rgb(col(0))] + [tuple(int(round(x)) for x in c) for c in lcent]
    # light tiles: add-value bytes (S * level), hflip baked; map entries index | vflip
    ltiles, lidx, lmap, lbytes = [], {}, [], {}
    for e in lents:
        t_, p, hf, vf = e & 0x3FF, (e >> 10) & 7, (e >> 14) & 1, (e >> 15) & 1
        key = (t_, p, hf)
        if key not in lidx:
            tsz = 16 if lbpp == 2 else 32
            off = (lb['name'] * 2 + t_ * tsz) & 0xFFFF
            px = (decode_2bpp_tile if lbpp == 2 else decode_4bpp_tile)(bytes(vram[(off + i) & 0xFFFF] for i in range(tsz)), 0)
            rows = [px[r * 8:r * 8 + 8] for r in range(8)]
            if hf:
                rows = [r[::-1] for r in rows]
            base = p * (4 if lbpp == 2 else 16)
            if vpair:
                data = bytes(S * (v >> 1) for r in rows for v in r)
            else:
                data = bytes(S * lvl_of_col[col(base + v)] if v else 0 for r in rows for v in r)
            # share identical tiles (also up to a vertical flip: the map entry carries vflip)
            vdata = b''.join(data[r * 8:r * 8 + 8] for r in range(7, -1, -1))
            if data in lbytes:
                lidx[key] = (lbytes[data], 0)
            elif vdata in lbytes:
                lidx[key] = (lbytes[vdata], 1)
            else:
                lbytes[data] = len(ltiles)
                lidx[key] = (len(ltiles), 0)
                ltiles.append(data)
        ti, fl = lidx[key]
        lmap.append(ti | ((vf ^ fl) << 15))
    # ---- palette banks
    def blend(m, sc):
        if cfg['op'] == 'addhalf':
            return tuple((a + b) >> 1 for a, b in zip(m, sc))
        if cfg['op'] == 'add':
            return tuple(min(a + b, 31) for a, b in zip(m, sc))
        raise ValueError(cfg['op'])
    # no priority bit: the Genesis plane A border (black, columns 0-31 / 288-319) covers the 32X there,
    # so the line compositor may write past the SNES's 256-pixel window
    pal = [0] * 256
    for k in range(K):
        for q in range(S):
            r, g, b = blend(levels[k], qcol[q])
            pal[k * S + q] = r | g << 5 | b << 10
    for q in range(S):                                   # unlit bank (performance mode BG1)
        r, g, b = qcol[q]
        pal[K * S + q] = r | g << 5 | b << 10
    assert K * S + S <= 253
    pal[253] = 0x83FF | 0x8000
    pal[254] = 0x8000
    pal[255] = 0xFFFF
    sky = [pal[0] & 0x7FFF] * 224
    err = sum(sum(abs(a - b) for a, b in zip(rgb(c), qcol[qof[c]])) for c in ucols) / max(1, len(ucols))
    print('   comp %02X: scene %d colours -> %d (mean err %.2f), light %d colours -> %d levels, %d light tiles'
          % (lv['entrance'], len(ucols), S - 1, err, len(lcols), K - 1, len(ltiles)))
    t_scroll = t['scroll']
    base_ov = dict(layer=cfg['base'], w=wid, h=hgt, map=base_map, scroll=t_scroll[cfg['base']], role='base', remap=remap)
    light_ov = dict(layer=cfg['light'], w=lw, h=lh, map=lmap, scroll=cfg.get('lscroll') or t_scroll.get(cfg['light'], (0, 0, 0, 0, 0, None)),
                    role='light', ltiles=ltiles)
    if cfg.get('wave') == 'misty':
        # run-time data: sway table (DATA_808BA8), scene colours, light level -> 2v representative
        light_ov['wave'] = bytes(L.rom.read(0x808BA8, 64))
        light_ov['qcol'] = [r | g << 5 | b << 10 for r, g, b in qcol]
        light_ov['vrep2'] = [4 * k + 1 for k in range(K)]          # pair (2k, 2k+1) -> 2v = 4k + 1
    return [base_ov, light_ov], pal, sky


HUD_CHARS = "0123456789FPSMX:%/ Y"
HUD_BITS = [0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF,
            0x79A4, 0x7BE4, 0x79CF, 0x5FED, 0x5AAD, 0x0410, 0x52A5, 0x12A4, 0x0000, 0x5AA4]


def hud_font_tiles():
    """3x5 HUD font at 2x in 8x16 cells (two 4bpp tiles per char): colour 2 on colour 1"""
    out = []
    for bits in HUD_BITS:
        cell = [[1] * 8 for _ in range(16)]
        for y in range(5):
            for x in range(3):
                if (bits >> (14 - (y * 3 + x))) & 1:
                    for dy in range(2):
                        for dx in range(2):
                            cell[3 + y * 2 + dy][1 + x * 2 + dx] = 2
        for half in range(2):
            t = bytearray()
            for r in cell[half * 8:half * 8 + 8]:
                for i in range(0, 8, 2):
                    t.append(r[i] << 4 | r[i + 1])
            out.append(bytes(t))
    return out


def perf_variant(L, lv, vram, cg, ts, t, grid, gw, gh, cx0, cy0):
    """Comp levels, performance mode: BG1 on Genesis plane A (map streamed by the 68000 from a grid of
    4x4-tile chunks), the background on plane B, and the transparency layer on the 32X as a
    checkerboard-dithered canvas (half the pixels show the layer's colour - or black where it is
    transparent - the other half the Genesis scene: the average is (layer + scene) / 2, exactly the
    SNES half-add).  Returns (md2 extras) and the 32X fog block."""
    cfg = COMP[lv['entrance']]
    rom = L.rom
    inv_pool = {v: k for k, v in ts.pool_idx.items()}
    used = sorted(set(grid))
    local = {v: i for i, v in enumerate(used)}
    vwords = []
    uniq = []
    useen = set()
    for v in used:
        ws = []
        for be in ts.vchunks[v]:
            pidx, pal16, hf = ts.plain(be)
            tnum = inv_pool[pidx][2]
            w = tnum | ((pal16 // 4) << 10) | (hf << 14) | ((be >> 15) << 15)
            ws.append(w)
            if (w & 0x1FFF) not in useen:
                useen.add(w & 0x1FFF)
                uniq.append(w & 0x1FFF)
        vwords.append(ws)
    rows = [uniq[i:i + 64] for i in range(0, len(uniq), 64)]
    rows[-1] = rows[-1] + [0] * (64 - len(rows[-1]))
    base_layer = cfg['base']
    gtiles, gmaps, gcram, _ = genesis_planes(vram, cg, lv, [('A', 1), ('B', base_layer)], None, None,
                                             ents_override={'A': rows}, noshadow=True)
    rawA = gmaps['A']['raw']
    gword = {}
    for i, w in enumerate(uniq):
        gword[w] = rawA[i] & ~0x1800                     # flips added per use
    vch = []
    for ws in vwords:
        for w in ws:
            g = gword[w & 0x1FFF]
            if g:
                if w & 0x4000:
                    g |= 0x0800
                if w & 0x8000:
                    g |= 0x1000
            vch.append(g)
    lgrid = [local[v] for v in grid]
    del gmaps['A']
    if cfg['op'] == 'add':
        return pf_add_variant(L, lv, vram, cg, cfg, gtiles, gmaps, gcram, lgrid, vch, gw, gh, cx0, cy0, len(used))
    # ---- 32X layer: BG(light) 2bpp -> 8bpp tiles, colour 0 -> black (index 1), palette p colour v -> 2 + 3p + v - 1
    bg = lv['bgs'][cfg['light'] - 1]
    assert bg['bpp'] == 2 and not bg['scs']
    fog_tiles, fog_idx, fog_map = [], {}, []
    for ty in range(32):
        for tx in range(32):
            a = map_addr(bg['sc'], bg['scs'], tx, ty) * 2
            e = vram[a] | vram[a + 1] << 8
            tn, pl, hf, vf = e & 0x3FF, (e >> 10) & 7, (e >> 14) & 1, (e >> 15) & 1
            off = (bg['name'] * 2 + tn * 16) & 0xFFFF
            px = decode_2bpp_tile(bytes(vram[(off + i) & 0xFFFF] for i in range(16)), 0)
            out = bytearray(64)
            for y in range(8):
                for x in range(8):
                    v = px[y * 8 + (7 - x if hf else x)]
                    out[y * 8 + x] = 1 if v == 0 else 2 + pl * 3 + v - 1
            k = bytes(out)
            if k not in fog_idx:
                fog_idx[k] = len(fog_tiles)
                fog_tiles.append(k)
            fog_map.append(fog_idx[k] | (vf << 15))
    pal = [0] * 256
    pal[1] = 0x8000
    for pl in range(8):
        for v in range(1, 4):
            w = cg[(pl * 4 + v) * 2] | cg[(pl * 4 + v) * 2 + 1] << 8
            pal[2 + pl * 3 + v - 1] = w | 0x8000
    pal[254] = 0x8000
    pal[255] = 0xFFFF
    fog = be16(len(fog_tiles)) + be16(0) + b''.join(be16(v) for v in fog_map) + \
        b''.join(be16(c) for c in pal) + b''.join(fog_tiles)
    pf = dict(gtiles=gtiles, gmaps=gmaps, gcram=gcram, grid=lgrid, vch=vch, gw=gw, gh=gh,
              ox0=cx0 * 32, oy0=cy0 * 32)
    print('   perf variant: %d genesis tiles, %d chunks, fog %d tiles' % (len(gtiles), len(used), len(fog_tiles)))
    return pf, fog


BAYER4 = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]
FOG_W = 16          # additive fog: 'on' pixels are grey W (scaled by the fade-in at run time)


def pf_add_variant(L, lv, vram, cg, cfg, gtiles, gmaps, gcram, lgrid, vch, gw, gh, cx0, cy0, nused):
    """Additive fog (Misty Mine: scene + grey(v), v = the fog pixel value): ordered dither of a grey W
    at density g(v) / W, g = floor(31 v / 32).  For a dark scene the average is close to scene + g.
    The Bayer pattern is anchored to the fog layer; the second framebuffer uses the pattern shifted
    one pixel, so the two alternate.  Tiles are baked per (tile, palette, flips) and per phase:
    map entries are plain slots, phase 1 = slot + n."""
    bg = lv['bgs'][cfg['light'] - 1]
    assert not bg['scs']
    bpp = bg['bpp']
    tsz = 32 if bpp == 4 else 16
    keys, kidx, fog_map = [], {}, []
    for ty in range(32):
        for tx in range(32):
            a = map_addr(bg['sc'], bg['scs'], tx, ty) * 2
            e = vram[a] | vram[a + 1] << 8
            k = (e & 0x3FF, (e >> 14) & 1, (e >> 15) & 1)
            if k not in kidx:
                kidx[k] = len(keys)
                keys.append(k)
            fog_map.append(kidx[k])
    # per phase: each (tile, flips) baked as grey dots - fog value v >= 3 -> colour 1 + v on every other
    # pixel (the other phase in the other framebuffer)
    phases = []
    for ph in range(2):
        for tn, hf, vf in keys:
            off = (bg['name'] * 2 + tn * tsz) & 0xFFFF
            px = (decode_4bpp_tile if bpp == 4 else decode_2bpp_tile)(bytes(vram[(off + i) & 0xFFFF] for i in range(tsz)), 0)
            out = bytearray(64)
            for y in range(8):
                for x in range(8):
                    sx, sy = (7 - x if hf else x), (7 - y if vf else y)
                    v = px[sy * 8 + sx]
                    # (the darkest fog values add almost nothing: left out, keeps the scene clean)
                    out[y * 8 + x] = 1 + v if v >= 3 and not ((x + y + ph) & 1) else 0
            phases.append(bytes(out))
    # CRAM 1 + v: the dot grey for fog value v (2 x the SNES add: the dots cover half the pixels)
    pal = [0] * 256
    for v in range(1, 16):
        g = min(31, 2 * ((31 * v) >> 5) + 3)
        pal[1 + v] = 0x8000 | g | g << 5 | g << 10
    pal[254] = 0x8000
    pal[255] = 0xFFFF
    sway = bytes(L.rom.read(0x808BA8, 64))
    fog = (be16(len(keys)) + be16(1) + b''.join(be16(v) for v in fog_map) +
           b''.join(be16(c) for c in pal) + b''.join(phases) + sway)
    pf = dict(gtiles=gtiles, gmaps=gmaps, gcram=gcram, grid=lgrid, vch=vch, gw=gw, gh=gh,
              ox0=cx0 * 32, oy0=cy0 * 32)
    print('   perf variant (add): %d genesis tiles, %d chunks, fog %d x 2 tiles' % (len(gtiles), nused, len(keys)))
    return pf, fog


GLOW_IDX = 252


def glimmer_block(L, lv, vram, cg):
    """Slipslide Ride glimmer (BG3 'NintendoLogoShine' glow added to the scene, CGADSUB $33): BG3 map
    drawn as a 32X overlay, a 50% checkerboard (the other phase in the other framebuffer) whose dots
    are scene + 2 x glow: BG3 palette p, shade v -> CRAM GLOW0 + 3 p + v - 1, set per fade step by the
    SH-2 from the mean scene colour and the shade's peak brightness (BG3 palettes 0-3 are grey ramps
    faded in and out over 8 frames by the game).  Block: u16 n, u16 kind 2, u16 map[1024] (key
    index), n tiles x 64, u16 mean scene colour, u16 peak[12]."""
    bg = lv['bgs'][2]
    assert bg['bpp'] == 2 and not bg['scs']
    peak = [[0, 1, 3, 5], [0, 5, 7, 9], [0, 9, 15, 17], [0, 19, 23, 30]]
    keys, kidx, gmap = [], {}, []
    for ty in range(32):
        for tx in range(32):
            a = map_addr(bg['sc'], bg['scs'], tx, ty) * 2
            e = vram[a] | vram[a + 1] << 8
            k = (e & 0x3FF, (e >> 10) & 7, (e >> 14) & 1, (e >> 15) & 1)
            if k not in kidx:
                kidx[k] = len(keys)
                keys.append(k)
            gmap.append(kidx[k])
    tiles = []                      # (the SH-2 masks every other pixel as it draws them)
    for tn, pl, hf, vf in keys:
        off = (bg['name'] * 2 + tn * 16) & 0xFFFF
        px = decode_2bpp_tile(bytes(vram[(off + i) & 0xFFFF] for i in range(16)), 0)
        out = bytearray(64)
        for y in range(8):
            for x in range(8):
                v = px[(7 - y if vf else y) * 8 + (7 - x if hf else x)]
                if v and (pl & 3):           # (palette 0 = the faint halo, adds 1-5: left out - a
                    out[y * 8 + x] = GLOW0 + (pl & 3) * 3 + v - 1   # dot over it reads as haze)
        tiles.append(bytes(out))
    # mean scene colour: the BG2 cave backdrop (the bulk of what the glimmers pass over)
    b2 = lv['bgs'][1]
    tsz = 32 if b2['bpp'] == 4 else 16
    acc, cnt = [0, 0, 0], 0
    for ty in range(32):
        for tx in range(32):
            a = map_addr(b2['sc'], b2['scs'], tx, ty) * 2
            e = vram[a] | vram[a + 1] << 8
            off = (b2['name'] * 2 + (e & 0x3FF) * tsz) & 0xFFFF
            px = (decode_4bpp_tile if b2['bpp'] == 4 else decode_2bpp_tile)(bytes(vram[(off + i) & 0xFFFF] for i in range(tsz)), 0)
            base = ((e >> 10) & 7) * (16 if b2['bpp'] == 4 else 4)
            for v in px:
                if v:
                    w = cg[(base + v) * 2] | cg[(base + v) * 2 + 1] << 8
                    for j in range(3):
                        acc[j] += (w >> (5 * j)) & 31
                    cnt += 1
    mean = sum(min(31, (acc[j] + cnt // 2) // max(1, cnt)) << (5 * j) for j in range(3))
    peaks = [peak[p][v] for p in range(4) for v in (1, 2, 3)]
    print('   glimmer: %d tiles, mean scene %04X' % (len(keys), mean))
    return (be16(len(keys)) + be16(2) + b''.join(be16(v) for v in gmap) + b''.join(tiles) +
            be16(mean) + b''.join(be16(v) for v in peaks))


def glimmer_genesis(L, lv, vram, tile0):
    """Slipslide glimmer on the Genesis: the BG3 glow map (256 x 256, palettes 1-3 = the sparkles;
    palette 0's faint halo left out) as S/H highlight sprites (palette 3 colour 14) over the BG2
    crystals - real brightening of the scene pixels; ordered-dither density = the shade's peak / 30.
    32 x 32 blocks, positioned by the 68000's own glimmer timer (glow_frame's logic).  Returns
    dict(blocks=[(x, y, tile)], tiles=[...])."""
    bg = lv['bgs'][2]
    peak = [[0, 1, 3, 5], [0, 5, 7, 9], [0, 9, 15, 17], [0, 19, 23, 30]]
    img = [[0] * 256 for _ in range(256)]
    for ty in range(32):
        for tx in range(32):
            a = map_addr(bg['sc'], bg['scs'], tx, ty) * 2
            e = vram[a] | vram[a + 1] << 8
            off = (bg['name'] * 2 + (e & 0x3FF) * 16) & 0xFFFF
            px = decode_2bpp_tile(bytes(vram[(off + i) & 0xFFFF] for i in range(16)), 0)
            p, hf, vf = (e >> 10) & 3, (e >> 14) & 1, (e >> 15) & 1
            for y in range(8):
                for x in range(8):
                    v = px[(7 - y if vf else y) * 8 + (7 - x if hf else x)]
                    if v and p:
                        img[ty * 8 + y][tx * 8 + x] = peak[p][v]
    blocks, tiles = [], []
    for by in range(0, 256, 32):
        for bx in range(0, 256, 32):
            px = [[BEAM_PIXV if img[by + y][bx + x] * 16 > (BAYER4[y & 3][x & 3] * 2 + 1) * 15 else 0
                   for x in range(32)] for y in range(32)]
            if not any(any(r) for r in px):
                continue
            blocks.append((bx, by, tile0 + len(tiles)))
            for tx in range(4):
                for ty in range(4):
                    tiles.append(bytes((px[ty * 8 + y][tx * 8 + x] << 4) | px[ty * 8 + y][tx * 8 + x + 1]
                                       for y in range(8) for x in range(0, 8, 2)))
    print('   glimmer (Genesis): %d highlight sprites, %d tiles' % (len(blocks), len(tiles)))
    return dict(blocks=blocks, tiles=tiles)


LAMP_SPR = {0xD9: (0xFDEE64, 0xFDEFBC), 0x30: (0xFDB658, 0xFDB9C0), 0x2B: (0xFDAEB8, 0xFDB2B0)}
LAMP_SCRIPT = 0xC5DF          # hanging lamp object (sprite spawn script) in the level sprite lists
LAMP_DOT, LAMP_PAL0, LAMP_C = 250, 238, 31
PLANE_DITHER = int(os.environ.get('PLANE_DITHER', '1'))   # (I19) Genesis planes: ordered dither in-line
PD_MIN_ERR = float(os.environ.get('PD_MIN_ERR', '150'))   # (weighted squared 8-bit error: nearest is good enough)
PD_GAIN = float(os.environ.get('PD_GAIN', '0.6'))         # (the mix must be this much better than the nearest)
PD_SPREAD = float(os.environ.get('PD_SPREAD', '0.03'))    # (penalty for mixing far-apart colours: grain)
LAMP_BRIGHT5 = 17             # (I18) lamp-level BG2 cells with a colour channel above this: normal (lanterns)
LAMP_NSH = 8                  # lamp shade colours (32X CRAM 128, 144, .. 240)
LAMP_LV0 = 228                # cone light levels 1-10 -> 32X CRAM 228-237 (checkerboard, flipped per frame)
GLOW0 = 240                   # glimmer: BG3 palette p, colour v -> 32X CRAM 240 + p * 3 + v - 1


def bg1_opaque_fn(ts, grid, gw, gh, cx0, cy0):
    """(level x, y) -> True where the BG1 playfield has a non-transparent pixel."""
    def f(wx, wy):
        cx, cy = (wx >> 5) - cx0, (wy >> 5) - cy0
        if wx < 0 or wy < 0 or not (0 <= cx < gw and 0 <= cy < gh):
            return False
        be = ts.vchunks[grid[cy * gw + cx]][((wy >> 3) & 3) * 4 + ((wx >> 3) & 3)]
        if (be & 0x7FFF) >= len(ts.bake):
            return False
        pidx, pal16, hf = ts.plain(be)
        x, y = wx & 7, wy & 7
        if hf:
            x = 7 - x
        if be & 0x8000:
            y = 7 - y
        b = ts.pool[pidx][y * 4 + (x >> 1)]
        return bool(b >> 4 if not (x & 1) else b & 15)
    return f


def lamp_block(L, lv, bg1_opaque=None, vram=None, cg=None):
    """Hanging lamps (walkway levels): the lamp shade + its additive blue light cone (BG3 on the main
    screen, scene on the sub screen, CGADSUB $24), baked into the 32X canvas by the SH-2: shade
    opaque, cone = a 50% checkerboard of dots (scene + 2 x light) where BG1 is transparent.
    Cone/shade come from a snes9x capture of the user's ROM (tools/capture/lamp_capture.py ->
    build/lamp.npz); lamp positions from the level's sprite list.  Block kind 3: u16 0, 3, nlamps,
    0, (x, y)[nlamps], u16 pal[12], lvl[10] (mean unlit scene per light level), light colour w[3]
    (1/16 per channel), u8 image[rows][176] (shade colour index, 0 empty; rows = header word 3)."""
    import numpy as np
    npz = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'lamp.npz')
    if not os.path.exists(npz):
        print('   (no lamp capture: run tools/capture/lamp_capture.py)')
        return None
    z = np.load(npz)
    blue, off, lx = z['blue'], z['off'], int(z['lx'])
    rom = L.rom
    a, b = LAMP_SPR[lv['entrance']]
    d = rom.read(a, b - a)
    lamps = []
    for j in range(0, len(d) - 7, 8):
        t, x, y, sc = (d[j + k] | d[j + k + 1] << 8 for k in (0, 2, 4, 6))
        if t and sc == LAMP_SCRIPT:
            # cone origin (x - 91, 481 - y) snapped to the 8-pixel grid: the Genesis cone is plane
            # cells, the 32X shade moves with it (by at most 4 pixels)
            ox, oy = ((x - 91 + 4) >> 3) << 3, ((481 - y + 4) >> 3) << 3
            lamps.append((ox + 91, 481 - oy))
    W, H, XO = 176, 224, 23 - (lx - 68)              # image x = screen x + XO (capture)
    cx2 = 2 * (lx - 3.5)                             # cone centre * 2 (mirror axis)
    L5 = np.zeros((H, W), int)
    for iy in range(H):
        for ix in range(W):
            sx = ix - XO
            if sx < 0:
                sx = int(cx2 - sx)
            if 0 <= sx < 256:
                L5[iy, ix] = blue[iy, sx]
    # lamp shade: bright pixels near the top of the 'off' frame
    shade = {}
    for iy in range(0, 32):
        for sx in range(max(0, lx - 26), min(256, lx + 14)):
            c = off[iy, sx]
            if c.sum() > 60:
                shade[(iy, sx + XO)] = tuple(int(v) >> 3 for v in c)
    # (8 shade colours at 32X CRAM 128 + 16 k: the sprite palettes' colour 0, never drawn - every
    #  other CRAM entry belongs to the level's BG1 or the game's sprite palettes in game mode)
    cols = sorted(set(shade.values()))
    if len(cols) > LAMP_NSH:
        cents, asg = _kmeans(cols, LAMP_NSH)
        cmap = {c: tuple(int(round(v)) for v in cents[asg[i]]) for i, c in enumerate(cols)}
        cols = sorted(set(cmap.values()))
        shade = {k: cmap[v] for k, v in shade.items()}
    cidx = {c: 128 + 16 * i for i, c in enumerate(cols)}
    pal = [0x8000 | r | g << 5 | bb << 10 for r, g, bb in cols] + [0] * (12 - len(cols))
    tlev = [[max(1, int(round(L5[ty * 8:ty * 8 + 8, tx * 8:tx * 8 + 8][L5[ty * 8:ty * 8 + 8, tx * 8:tx * 8 + 8] > 0].mean())))
             if (L5[ty * 8:ty * 8 + 8, tx * 8:tx * 8 + 8] > 0).any() else 1 for tx in range(W // 8)] for ty in range(H // 8)]
    img = np.zeros((H, W), int)                  # the 32X: the shade (the cone is on the Genesis)
    for (iy, ix), c in shade.items():
        img[iy, ix] = cidx[c]
    HS = (max(iy for iy, ix in shade) + 8) & ~7 if shade else 8   # 32X image rows (the shade)
    img = img[:HS]
    blk = be16(0) + be16(3) + be16(len(lamps)) + be16(HS)
    blk += b''.join(be16(x) + be16(y) for x, y in lamps)
    blk += b''.join(be16(c) for c in pal)
    # light level colours: mean unlit scene under the pixels of that level + 2 x the added blue
    lvcol = []
    for k in range(1, 11):
        sel = []
        for iy in range(H):
            for ix in range(W):
                if L5[iy, ix] == k or (k == 10 and L5[iy, ix] > 10):
                    sx = ix - XO
                    if 0 <= sx < 256 and off[iy, sx].sum() < 90:   # the dark cave behind (dots only
                        sel.append(off[iy, sx])                       # land where BG1 is transparent)
        m = np.mean(sel, axis=0) / 8 if sel else np.array([1.0, 1.0, 2.0])
        r, g, bb = (int(round(v)) for v in m)          # (the SH-2 adds 2 x the flickering light)
        lvcol.append(min(31, r) | min(31, g) << 5 | min(31, bb) << 10)
    # light colour (1/16 units per channel; Winky's Walkway and Tanked Up Trouble blue, Platform Perils
    # warm) and, for the other levels, their own unlit scene under the cone (from their capture)
    w = (0, 0, 16)
    e = lv['entrance']
    if 'on_%02X' % e in z:
        on_, off_ = z['on_%02X' % e] // 8, z['off_%02X' % e] // 8
        dd = np.clip(on_ - off_, 0, 31)
        lit = dd.sum(2) > 0
        tot = dd[lit].sum(0).astype(float)
        w = tuple(int(round(16 * v / tot.max())) for v in tot)
        sel = off_[lit & (off_.sum(2) < 12)]
        m = sel.mean(0) if len(sel) else np.zeros(3)
        c = int(round(m[0])) | int(round(m[1])) << 5 | int(round(m[2])) << 10
        lvcol = [c] * 10
    print('   lamp light %s, scene %s' % (w, [(c & 31, (c >> 5) & 31, c >> 10) for c in lvcol[:3]]))
    blk += b''.join(be16(c) for c in lvcol) + b''.join(be16(v) for v in w)
    blk += bytes(int(v) for v in img.ravel())                                       # [HS][176]
    # the cone on Genesis plane B (in front of BG2 on plane A, behind the 32X BG1): a 50% checkerboard
    # of dots in colour 1-10 of line 3 (per-pixel light level), the other phase every other frame by
    # a 1-pixel H scroll; the lamp's swing = per-line H scroll (a skew about row 25)
    ctiles, cmap = [], []
    for ty in range(H // 8):
        for tx in range(W // 8):
            t = bytearray(32)
            any_ = False
            for y in range(8):
                for x in range(8):
                    iy, ix = ty * 8 + y, tx * 8 + x
                    v = min(10, int(L5[iy, ix]))
                    # (levels 1-2 add at most 2 x 2 and nothing at all with the flicker's d: their
                    # dots would only show the scene mean - left out, the faint halo goes with them)
                    if v >= 3 and not ((ix + iy) & 1):
                        t[y * 4 + (x >> 1)] |= v << (0 if x & 1 else 4)
                        any_ = True
            if any_:
                cmap.append(len(ctiles) + 1)
                ctiles.append(bytes(t))
            else:
                cmap.append(0)
    gcols = []                                      # [flicker d 0-3][level 1-10], as lamp_frame()
    for d in range(4):
        for k in range(10):
            l, c = k + 1 - d, lvcol[k]
            rgb = [((c >> (5 * j)) & 31) + ((2 * l * w[j]) >> 4 if l > 0 else 0) for j in range(3)]
            gcols.append(gen_level([min(31, v) for v in rgb], True))     # (I18: shadowed, S/H on)
    # swing (snes9x: skew about row 25, 126-frame period, -14..+18 px at row 140 from the captured
    # pose): per-row slope in 1/256 px
    # The SNES swing (CODE_80B891, the BG3 H-scroll HDMA table at 7F:0200/0500, built one frame
    # ahead): p = ($28 * 2) & 0x7F folded at 0x40, v = DATA_808BA8[p] / 2, negated (~v) when
    # ($28 * 2) & 0x80, W = v >> 1 (arith.); table line j (j = 0 at the apex, 24 rows below the
    # image origin) gets HOFS += (W >> 4) + j * W / 256 -> period 128 frames, phase = $28 - 1
    # (verified exactly against snes9x captures).  The 68K gets $28 - 1 as C_FRAME in lamp levels.
    swing = list(rom.read(0x808BA8, 64)) + [0] * 62          # (block layout: 126 words)
    cone = dict(lamps=[((x - 91) >> 3, (481 - y) >> 3) for x, y in lamps], map=cmap, tiles=ctiles,
                cols=gcols, swing=swing, pivot=24)
    print('   lamps: %d, %d shade colours, Genesis cone %d tiles' % (len(lamps), len(cols), len(ctiles)))
    return blk, cone


RAIN_LEVELS = {0x0C: (0xFD8AD4, 0xFD8D7C),    # entrance -> sprite list (holds the weather triggers)
               0x1C: (0, 0), 0x1B: (0, 0)}    # (Ropey's bonus rooms: storm, no triggers)
# jungle levels with a darkness trigger: Ropey (night -> day at its end), Jungle Hijinxs and Orang-utan
# Gang (day -> the storm-night palette, the sun sets near Jungle Hijinxs' end)
JUNGLE_WX = {0x0C: (0xFD8AD4, 0xFD8D7C), 0x0D: (0xFD8D7C, 0xFD90C4), 0x16: (0xFD95DC, 0xFD981C),
             0x1C: (0, 0), 0x1B: (0, 0)}


def hdma_colours(data):
    """An HDMA colour table (count, 2 bytes, colour) expanded to 224 lines."""
    out, i = [], 0
    while i + 4 < len(data) and len(out) < 224:
        n = data[i]
        if n == 0:
            break
        out += [data[i + 3] | data[i + 4] << 8] * (n & 0x7F)
        i += 5
    while len(out) < 224:
        out.append(out[-1] if out else 0)
    return out[:224]


def jungle_sunset(e):
    """Orang-utan Gang's sunset (captured: build/weather.npz) -> (cg bytes, sky[224], bg3 haze[224])"""
    snow_keys(0x24)                       # loads the npz
    if 'sunset_%02X_pal' % e not in _WX_NPZ:
        return None
    pal = [int(c) for c in _WX_NPZ['sunset_%02X_pal' % e]]
    cg = b''.join(bytes([c & 0xFF, c >> 8]) for c in pal)
    return cg, hdma_colours(bytes(_WX_NPZ['sunset_%02X_sky' % e])), hdma_colours(bytes(_WX_NPZ['sunset_%02X_bg3' % e]))


def jungle_night(L, cg):
    """The darkened jungle palette (CODE_80AC8B: colours 16-127 of DATA_B9A1DC -> r*5/16, g*5/8,
    blue = that green) and its sky (lines 0-70 DATA_B99658, the rest black)."""
    rom = L.rom
    out = bytearray(cg)
    for i in range(16, 128):
        R, G, B = _ch(rom.u16(0xB9A1DC + i * 2))
        r2 = ((R << 8) >> 2) + ((R << 8) >> 4)
        g = G << 5
        g2 = ((g >> 1) + (g >> 3)) & 0x3E0
        w = (r2 >> 8) | g2 | (g2 << 5)
        out[i * 2], out[i * 2 + 1] = w & 0xFF, w >> 8
    sky = [rom.u16(0xB99658 + i * 2) for i in range(71)] + [0] * (224 - 71)
    return bytes(out), sky
RAIN_IDX = 245                    # 32X CRAM 245-247: rain colours
# weather trigger entries (type 8) in the sprite lists: routine (bank BD) -> what it does to $1DF3
WX_ROUTINES = {0xFD3C: 'rain_on', 0xFD46: 'rain_off', 0xFD50: 'dark_on', 0xFD63: 'dark_off',
               0xFDA9: 'flash_on', 0xFDB3: 'flash_off', 0xFD76: 0, 0xFD7D: 1, 0xFD84: 2, 0xFD8B: 3,
               0xFD92: 4, 0xFD99: 5}


def weather_triggers(rom, a, b):
    """Type-8 entries of a sprite list: (x, y, action).  The game runs the routine when the entry
    enters the spawn window around the camera."""
    out = []
    for o in range(a, b, 8):
        t, x, y, r = (rom.u16(o + i) for i in (0, 2, 4, 6))
        if t == 8 and r in WX_ROUTINES:
            out.append((x, y, WX_ROUTINES[r]))
    return out


def rain_block(L, lv, vram, cg, wx, rain=True):
    """Ropey Rampage rain: BG3 (map at VRAM $2C00, priority over BG1) as a pixel list, drawn on the
    32X over the canvas each frame with the overwritten bytes saved for restoring.  The game scrolls
    it by (+8, -8) per frame on top of the camera (NMI 808D43: HOFS = camx + $1DF5, VOFS = $1DF7).
    Rain off (CODE_818FF2): the map switches to $2800 (empty) stacked above, so the rain's top edge
    sweeps down 8 px a frame for 32 frames.  Darkness off (CODE_B6B324 with $1DFD = $20): the palette
    fades linearly back to the day jungle palette in 32 steps of 8 frames.
    Block kind 4: u16 n, 4, rain_off_x, dark_change_x, col[3], bg3 gradient -> black (day -> night),
                  genesis CRAM night[64], day[64], 32X day palette[128], day sky[224],
                  u16 row start[257], (u16 x | colour - 245 << 8)[n] sorted by row."""
    bg = lv['bgs'][2]
    pix = []
    for ty in range(32 if rain else 0):
        for tx in range(32):
            a = map_addr(bg['sc'], 0, tx, ty) * 2
            e = vram[a] | vram[a + 1] << 8
            off = (bg['name'] * 2 + (e & 0x3FF) * 16) & 0xFFFF
            px = decode_2bpp_tile(bytes(vram[(off + i) & 0xFFFF] for i in range(16)), 0)
            hf, vf = (e >> 14) & 1, (e >> 15) & 1
            for y in range(8):
                for x in range(8):
                    v = px[(7 - y if vf else y) * 8 + (7 - x if hf else x)]
                    if v:
                        pix.append((ty * 8 + y, tx * 8 + x, v))
    pix.sort()
    pl = 1
    cols = [0x8000 | (cg[(pl * 4 + v) * 2] | cg[(pl * 4 + v) * 2 + 1] << 8) for v in (1, 2, 3)]
    trig = weather_triggers(L.rom, *JUNGLE_WX[lv['entrance']])
    xo = [x for x, y, act in trig if act == 'rain_off']
    xd = [x for x, y, act in trig if act == ('dark_off' if rain else 'dark_on')]
    rain_off = xo[0] if xo else 0xFFFF
    dark_off = xd[0] if xd else 0xFFFF
    blk = be16(len(pix)) + be16(4) + be16(rain_off) + be16(dark_off)
    blk += b''.join(be16(c) for c in cols) + be16(0 if rain else 1)     # 1: BG3 gradient fades to black
    assert len(wx['gN']) == 64 and len(wx['gD']) == 64 and len(wx['palD']) == 128 and len(wx['skyD']) == 224
    blk += b''.join(be16(c) for c in wx['gN'] + wx['gD'] + wx['palD'] + wx['skyD'])
    # BG3 haze gradient target (SNES colours; the 68000 block converts) - 224 words
    sh3 = True
    gB = (lv.get('_gmaps') or {}).get('B')
    if gB is not None:
        sh3 = gB['shadow']
    blk += b''.join(be16(gen_level(apply_colormath(snes_rgb(c), lv['regs'], 3), sh3) if c else 0)
                    for c in wx['bg3E'])
    # per map row: start index (257 u16), then pixels u16 x | (colour - RAIN_IDX) << 8
    starts = [0] * 257
    for y, x, v in pix:
        starts[y + 1] += 1
    for r in range(256):
        starts[r + 1] += starts[r]
    blk += b''.join(be16(v) for v in starts)
    blk += b''.join(be16(x | (v - 1) << 8) for y, x, v in pix)
    if rain:                                  # lightning flash: Genesis CRAM (the 68000's 3rd keyframe)
        assert len(wx['gF']) == 64
        blk += b''.join(be16(c) for c in wx['gF'])
    print('   rain: %d pixels, rain off x %d, darkness change x %d' % (len(pix), rain_off, dark_off))
    return blk


# light-switch levels (NorSpr63 barrels -> $0579 bit $200 for the script's 1375 duration):
# Stop & Go Station: the backdrop colour added to the scene is green $0060 (GO) or red $0002 (STOP)
# (CODE_80B206); Loopy Lights: the fixed colour subtracted is 9 (lights off) or 3 (on) (CODE_80B38B)
LIGHTS = {0x31: dict(spr=(0xFDB9C0, 0xFDBE28), base=0xE6F1, keys=((0, 3, 0), (2, 0, 0))),
          0x36: dict(spr=(0xFDBEE8, 0xFDC320), base=0xE6D7, keys=((-9, -9, -9), (-3, -3, -3)), flicker=1),
          # Torchlight Trouble (CODE_808E38 type): colours 1-127 = DATA_B9A01C inverted, main = backdrop
          # (16,16,16) / Squawks' BG3 beam (30,30,31) minus that sub-screen -> outside the beam t - 15,
          # in it t - (1,1,0).  No per-pixel beam here: dark until Squawks' crate (x 29128), lit after.
          0x14: dict(spr=(0xFD9434, 0xFD95DC), fixed=[(29128, 0xFFFF)], src=0xB9A01C, no_ov=True,
                     keys=((-15, -15, -15), (-1, -1, 0)), beam=(64, 110))}
# (I14) Torchlight's bonus rooms (the barrel room, the claptrap room): the same dark palette and beam -
# their captures (with the beam on) gave the inverted colours (an icy white cave)
LIGHTS[0xC1] = LIGHTS[0xC3] = LIGHTS[0x14]
# Squawks' flashlight (Torchlight): the SNES window wedge from Squawks, top edge -0.2375 px/px, lower-left
# edge +0.25 px/px; the viewer has no Squawks, so the apex sits at a fixed screen spot (bobbing 3 px).
# lit(x, y) = x >= beam_L(y); the SH-2 uses the same integer formula (bm_L)
BEAM_K1, BEAM_K2 = 1078, 64
BEAM_PIX = int(os.environ.get('BEAMPIX', 14))   # (14 = highlight operator; another value draws the sprites, for tests;
BEAM_PIXV = 1 if BEAM_PIX == 99 else BEAM_PIX  #  99: beam tiles coloured full 3 / partial 1, 2)


def beam_L(ax, ay, y):
    d = y - ay
    return ax + max((-d * BEAM_K1) >> 8, (d * BEAM_K2) >> 8)


def beam_sprites(ax, ay, tile0):
    """Genesis S/H highlight sprites covering the wedge (plane B is shadowed everywhere; a highlight
    pixel - palette 3 colour 14 - over it gives normal brightness): 32 x 32 sprites, full ones share
    one 16-tile set.  Returns dict(sat=[(y, size_link, attr, x)], tiles=[32-byte tiles])."""
    Y1 = 240                                       # covers the bob (+-3) below line 224
    ytop = next(y for y in range(Y1) if beam_L(ax, ay, y) < 320) & ~7
    xs = min(beam_L(ax, ay, y) for y in range(Y1)) & ~31
    sets, sat = {}, []
    tiles = []
    for by in range(ytop, Y1, 32):
        for bx in range(max(0, xs), 320, 32):
            px = [[BEAM_PIXV if (bx + x >= beam_L(ax, ay, by + y) + 4 or
                                (bx + x >= beam_L(ax, ay, by + y) and not ((x + y) & 1))) else 0
                   for x in range(32)] for y in range(32)]     # (a 4-pixel checkerboard soft edge)
            if not any(any(r) for r in px):
                continue
            key = tuple(tuple(r) for r in px)
            key = (bx, by)       # every sprite its own tiles: the MiSTer core drew shared ones wrong
            if key not in sets:
                sets[key] = tile0 + len(tiles)
                for tx in range(4):                # sprite tiles run down the columns first
                    for ty in range(4):
                        q = [[px[ty * 8 + y][tx * 8 + x] for x in range(8)] for y in range(8)]
                        if BEAM_PIX == 99:         # (test: full tiles colour 3, partial ones 1 / 2)
                            full = all(all(r) for r in q)
                            q = [[(3 if full else 1 + ((tx + ty) & 1)) if v else 0 for v in r] for r in q]
                        tiles.append(bytes((q[y][x] << 4) | q[y][x + 1] for y in range(8) for x in range(0, 8, 2)))
            sat.append([by + 128, 0x0F00, 0x6000 | sets[key], bx + 128])
    for i, e in enumerate(sat):
        e[1] |= (i + 1) if i + 1 < len(sat) else 0
    print('   beam: %d highlight sprites, %d tiles' % (len(sat), len(tiles)))
    return dict(sat=sat, tiles=tiles)


def light_barrels(L, spr, base):
    """(x, duration) of the light-switch barrels: scripts '00 82 <base> 75 13 <duration> 00 80'."""
    rom = L.rom
    out = []
    for o in range(spr[0], spr[1], 8):
        t, x, y, r = (rom.u16(o + i) for i in (0, 2, 4, 6))
        sc = 0xB50000 | r
        if t == 1 and rom.u16(sc) == 0x8200 and rom.u16(sc + 2) == base and rom.u16(sc + 4) == 0x1375:
            out.append((x, rom.u16(sc + 6)))
    return sorted(out)


def light_cgs(cg, keys):
    """The level palette with each key's colour offset applied (clamped), as CGRAM bytes."""
    outs = []
    for d in keys:
        q = bytearray(cg)
        for i in range(128):
            R, G, B = _ch(cg[i * 2] | cg[i * 2 + 1] << 8)
            w = max(0, min(31, R + d[0])) | max(0, min(31, G + d[1])) << 5 | max(0, min(31, B + d[2])) << 10
            q[i * 2], q[i * 2 + 1] = w & 0xFF, w >> 8
        outs.append(bytes(q))
    return outs


MILL_FRAMES = 0xF95B5E          # DATA_F95B5E: 4 BG2 tilemaps of the rolling millstone (0x380 bytes each)


def millstone_block(L, lv, vram):
    """Temple levels (Millstone Mayhem / Temple Tempest): the millstone is BG2 = one 256 x 112
    picture (map 32 x 14 at VRAM $7000, tiles at $2000 words) placed by CODE_809278 from the
    tracked millstone ($DD, $DF): HOFS = camx + $80 - $DD, VOFS = $DF - $1C0 + camy; the picture
    turns by DMAing one of 4 tilemaps (DATA_F95B5E) every 4 frames.  The SH-2 draws it as 8x8
    pseudo-sprites under the game's sprites (sprites.c spr_ms_*).  Block kind 7: u16 0, 7, ntiles,
    palette, bbox c0, r0, c1, r1 (cells, end exclusive), frames[4][448] (tile | hflip << 14 |
    vflip << 15, 0xFFFF = empty), u32 opaque[4][14] (fully opaque cells), u8 span[4][14][2] (non-empty
    columns), tiles[ntiles][8] u32 rows (chunky nibbles, pixel 0 on top)."""
    if vram is None or len(vram) < 0x10000:
        return None
    fr = L.rom.read(MILL_FRAMES, 0x380 * 4)
    tiles, tidx, frames, pals, full = [], {}, [], set(), set()
    for k in range(4):
        out = []
        for i in range(448):
            e = fr[k * 0x380 + 2 * i] | fr[k * 0x380 + 2 * i + 1] << 8
            t = e & 0x3FF
            if t not in tidx:
                px = decode_4bpp_tile(vram[0x4000 + t * 32: 0x4000 + t * 32 + 32], 0)
                if not any(px):
                    tidx[t] = None
                else:
                    tidx[t] = len(tiles)
                    if all(px):
                        full.add(len(tiles))
                    tiles.append(bytes((px[2 * q] << 4) | px[2 * q + 1] for q in range(32)))
            j = tidx[t]
            if j is not None:
                pals.add((e >> 10) & 7)
            out.append(0xFFFF if j is None else j | (e >> 14 & 1) << 14 | (e >> 15 & 1) << 15)
        frames.append(out)
    if not tiles:
        return None
    assert len(pals) == 1, pals
    used = [i for i in range(448) if any(f[i] != 0xFFFF for f in frames)]
    c0, c1 = min(i % 32 for i in used), max(i % 32 for i in used) + 1
    r0, r1 = min(i // 32 for i in used), max(i // 32 for i in used) + 1
    blk = be16(0) + be16(7) + be16(len(tiles)) + be16(pals.pop()) + be16(c0) + be16(r0) + be16(c1) + be16(r1)
    blk += b''.join(be16(v) for f in frames for v in f)
    # cells opaque in all 4 tilemaps (u32 per row, bit = column): the SH-2 skips restoring the canvas
    # tiles the new picture covers completely
    opq = [[sum(1 << c for c in range(32) if f[r * 32 + c] != 0xFFFF and (f[r * 32 + c] & 0x3FFF) in full)
            for r in range(14)] for f in frames]
    blk += b''.join(be32(v) for o in opq for v in o)
    # per tilemap and row: the non-empty cell columns [first, end) (the canvas tiles to mark)
    for f in frames:
        for r in range(14):
            cs = [c for c in range(32) if f[r * 32 + c] != 0xFFFF]
            blk += bytes((cs[0], cs[-1] + 1) if cs else (0, 0))
    blk += b''.join(tiles)
    print('   millstone: %d tiles, cells %d..%d x %d..%d, %d opaque (map 0)' % (
        len(tiles), c0, c1, r0, r1, sum(bin(v).count('1') for v in opq[0])))
    return blk


def lights_block(L, lv, wx):
    """Block kind 6: u16 0, 6, nbarrels, flicker, nkey 2, shift 0, beam apex x, y (0: no beam),
    (u16 x, duration)[n], Genesis CRAM[2][64], 32X palette[2][128], sky colour[2] (key 0 = default,
    1 = switched), [beam levels: lit 32X palette[128]]."""
    li = LIGHTS[lv['entrance']]
    bar = li['fixed'] if 'fixed' in li else light_barrels(L, li['spr'], li['base'])
    bm = li.get('beam', (0, 0))
    blk = be16(0) + be16(6) + be16(len(bar)) + be16(li.get('flicker', 0)) + be16(2) + be16(0) + be16(bm[0]) + be16(bm[1])
    blk += b''.join(be16(x) + be16(d) for x, d in bar)
    blk += b''.join(be16(c) for k in range(2) for c in wx['gkeys'][k])
    blk += b''.join(be16(c) for k in range(2) for c in wx['pkeys'][k])
    blk += b''.join(be16(c) for c in wx['skeys'])
    if 'lit' in wx:                               # Squawks' beam: the lit 32X palette (CRAM 128 +)
        blk += b''.join(be16(c) for c in wx['lit'])
    print('   light barrels: %s' % bar)
    return blk


def heavy_snow_genesis(L, vram, hb):
    """The blizzard layer (BG3 map $7C00, 64 tiles x 8 frames from DATA_DECE74, CODE_808B1F) as Genesis
    data: tiles uploaded at hb.. every other frame, palette line 3 colours 1-3, the map for VRAM $6000
    (plane B shows it during the blizzard, high priority: in front of BG2 on plane A), and the sway
    table (DATA_808BA8)."""
    rom = L.rom
    m = []
    for ty in range(32):
        for tx in range(64):
            a = (0x7C00 + ty * 32 + (tx & 31)) * 2
            e = vram[a] | vram[a + 1] << 8
            m.append((hb + (e & 0x3F)) | 3 << 13 | (0x800 if e & 0x4000 else 0) | (0x1000 if e & 0x8000 else 0) | 0x8000)
    frames = []
    for f in range(SNOW_FRAMES):
        data = rom.read(0xDECE74 + 0x400 * f, 0x400)
        for t in range(64):
            px = decode_2bpp_tile(data, t * 16)
            frames.append(bytes((px[i] << 4) | px[i + 1] for i in range(0, 64, 2)))
    # the 32X's copy of the flakes (drawn only over BG1's opaque pixels, see the SH-2's rain phase):
    # per animation frame, per map row (256 x 256 pixels) the (x | (colour - 1) << 8) entries
    fl = []
    for f in range(SNOW_FRAMES):
        rows, ent = [], []
        data = rom.read(0xDECE74 + 0x400 * f, 0x400)
        dec = [decode_2bpp_tile(data, t * 16) for t in range(64)]
        for r in range(256):
            rows.append(len(ent))
            ty, y = r >> 3, r & 7
            for tx in range(32):
                e = m[ty * 64 + tx]
                px = dec[((e & 0x7FF) - hb) & 63]
                yy = 7 - y if e & 0x1000 else y
                for x in range(8):
                    v = px[yy * 8 + (7 - x if e & 0x800 else x)]
                    if v:
                        ent.append((tx * 8 + x) | (v - 1) << 8)
        rows.append(len(ent))
        fl.append(b''.join(be16(v) for v in rows + ent))
    return dict(hb=hb, map=m, frames=b''.join(frames), sway=rom.read(0x808BA8, 64), h=32, flakes=fl)


SNOW_WX = {0x24: dict(spr=(0xFDA7E0, 0xFDAC38), start=0),     # Snow Barrel Blast: 0 -> 5
           0xA7: dict(spr=(0xFDE152, 0xFDE3D2), start=5)}     # Ice Age Alley: 5 -> 0
_WX_NPZ = None


def snow_keys(e):
    """Weather palette keyframes of a snow level (tools/capture/weather_capture.py -> build/weather.npz),
    or None (level then keeps its fixed look)."""
    global _WX_NPZ
    if e not in SNOW_WX:
        return None
    if _WX_NPZ is None:
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'weather.npz')
        import numpy as np
        _WX_NPZ = dict(np.load(path)) if os.path.exists(path) else {}
    if 'pal_%02X' % e not in _WX_NPZ:
        return None
    return dict(SNOW_WX[e], pal=_WX_NPZ['pal_%02X' % e])


def snow_block(L, lv, snow, wx):
    """Block kind 5 (weather states of a snow level): u16 0, 5, start state, ntrig, nkey 6, step shift
    3 (8 steps of 8 frames per fade), 0, 0, (u16 x, state)[ntrig] sorted by x, Genesis CRAM[6][64],
    32X palette[6][128], sky colour[6], then the blizzard flakes for the 32X (see below)."""
    trig = sorted((x, a) for x, y, a in weather_triggers(L.rom, *snow['spr']) if isinstance(a, int))
    blk = be16(0) + be16(5) + be16(snow['start']) + be16(len(trig)) + be16(6) + be16(3) + be16(0) + be16(0)
    blk += b''.join(be16(x) + be16(a) for x, a in trig)
    blk += b''.join(be16(c) for k in range(6) for c in wx['gkeys'][k])
    blk += b''.join(be16(c) for k in range(6) for c in wx['pkeys'][k])
    blk += b''.join(be16(c) for c in wx['skeys'])
    if wx.get('flakes'):
        # the blizzard's flakes for the 32X: u16 1, colours[3], sway[64] bytes, list sizes (words)[8],
        # lists[8] (rain format: row starts[257], entries)
        fl = wx['flakes']
        blk += be16(1) + b''.join(be16(c) for c in wx['flakecols']) + bytes(wx['sway'])
        blk += b''.join(be16(len(x) // 2) for x in fl) + b''.join(fl)
    print('   weather: start %d, triggers %s' % (snow['start'], trig))
    return blk


def build_level(L, lv, tilesets):
    rom = L.rom
    t = TYPES[lv['nmi']]
    vram, cg = build_vram(L, lv)
    cg = survey_cgram(lv, cg)
    cg_day = cg
    if lv['entrance'] in JUNGLE_WX:       # day jungle palette (the storm's fade source, = snes9x after it)
        cg_day = bytearray(cg)
        for i in range(17, 128):
            w = L.rom.u16(0xB9A1DC + i * 2)
            cg_day[i * 2], cg_day[i * 2 + 1] = w & 0xFF, w >> 8
        cg_day = bytes(cg_day)
    cg, sky_override = runtime_palette(L, lv, cg)
    play = t['play']
    pbg = lv['bgs'][play - 1]
    vert = lv['stream'] in (0x8168, 0x831B)
    tskey = (lv['layout'], pbg['name'])
    ts = tilesets.setdefault(tskey, Tileset(tskey))
    # ---- playfield grid
    if vert:
        rows_total = (0xE9C380 - 0xE90000) // 128 if lv['layout'] == 0xE90000 else (0xE13BE5 - 0xE101E5) // 128
        cx0, cx1, cy0, cy1 = 0, 64, 0, rows_total
    else:
        cx0 = lv['x_min'] // 32
        cx1 = min((lv['x_max'] + 320 + 31) // 32 + 1, 0x7FFF)
        if lv['x_min'] == lv['x_max']:
            # single-screen boss arenas (Necky's Nuts, Dumb Drum, Necky's Revenge): the SNES camera never
            # leaves its 256 pixels and the level data beyond them is garbage - the grid ends there
            # (the 32X shows nothing past it: the Genesis background fills the 32-pixel margins)
            cx1 = (lv['x_max'] + 256) // 32
        cy0, cy1 = 0, 16
    gw, gh = cx1 - cx0, cy1 - cy0
    grid = []
    for cy in range(cy0, cy1):
        for cx in range(cx0, cx1):
            ent = []
            pm = 0
            for sy in range(4):
                for sx in range(4):
                    e = layout_tile(rom, lv, cx * 4 + sx, cy * 4 + sy)
                    be = ts.entry(vram, pbg['name'], 4, e)
                    if lv['nmi'] == '8090C3':
                        # (H17) Mine Cart Carnage: BG2 = a second layout (+ 4B00, same scroll as BG1:
                        # CODE_818B6B / CODE_8188A8) - the bed between the rails; baked under BG1
                        e2 = layout_tile(rom, lv, cx * 4 + sx, cy * 4 + sy, base=lv['layout'] + 0x4B00)
                        be2 = ts.entry(vram, lv['bgs'][1]['name'], 4, e2)
                        if not tile_is_empty(ts, be2):
                            if tile_is_empty(ts, be):
                                be, e = be2, e2 & ~0x2000
                            elif not tile_is_full(ts, be):
                                be = ts.composite(be, be2)
                    ent.append(be)
                    if (e & 0x2000) and not tile_is_empty(ts, be):
                        pm |= 1 << (sy * 4 + sx)
            grid.append(ts.vchunk(ent, pm))
    # ---- 32X overlay layers
    overlays = []
    for layer, role in t['layers'].items():
        if role != 'ov' or LIGHTS.get(lv['entrance'], {}).get('no_ov'):
            continue
        bg = lv['bgs'][layer - 1]
        wid = 64 if bg['scs'] & 1 else 32
        hgt = 64 if bg['scs'] & 2 else 32
        m = []
        for ty in range(hgt):
            for tx in range(wid):
                a = map_addr(bg['sc'], bg['scs'], tx, ty) * 2
                e = vram[a] | vram[a + 1] << 8
                be = ts.entry(vram, bg['name'], bg['bpp'], e)
                if tile_is_empty(ts, be):
                    be = 0x7FFF
                m.append(be)
        overlays.append(dict(layer=layer, w=wid, h=hgt, map=m, scroll=t['scroll'][layer]))
    snow = snow_keys(lv['entrance'])
    if lv['entrance'] == 0xA7 and snow is None:  # (weather levels: the blizzard is on the Genesis)
        overlays.append(heavy_snow_overlay(L, lv, vram, ts, sc=0x7C00))
    comp = None
    perf = None
    if lv['entrance'] in COMP:
        grid_bakes = set()
        for v in set(grid):
            for be in ts.vchunks[v]:
                grid_bakes.add(be & 0x7FFF)
        overlays, comp_pal, comp_sky = comp_level(L, lv, vram, cg, ts, t, grid_bakes)
        comp = True
        perf = perf_variant(L, lv, vram, cg, ts, t, grid, gw, gh, cx0, cy0) if COMP[lv['entrance']].get('perf') else None
    # ---- palettes
    pal = [0] * 256
    for i in range(128):
        if i % 16:
            pal[i] = (cg[i * 2] | cg[i * 2 + 1] << 8) | 0x8000
    for i in range(128, 256):
        pal[i] = 0x8000
    pal[254] = 0x8000
    pal[255] = 0xFFFF
    sky = sky_override or sky_table(L, lv, cg)
    if comp:
        pal, sky = comp_pal, comp_sky
    # ---- Genesis planes
    planes = [(role, layer) for layer, role in t['layers'].items() if role in ('A', 'B')]
    if lv['entrance'] in LAMP_SPR:                       # walkway lamps: BG2 on plane B, the cone on A
        planes = [('B' if r == 'A' else r, l) for r, l in planes]
    if lv['entrance'] in RAIN_LEVELS:
        planes = [(r, l) for r, l in planes if l != 3]      # BG3 = rain, drawn on the 32X
    if any(o.get('anim') for o in overlays) and snow is None:
        planes = [(r, l) for r, l in planes if l != 3]      # BG3 is the 32X snow overlay
    if comp:
        planes = []                                          # whole scene is on the 32X
    anim = None
    ents_override = None
    if snow is not None:
        # weather snow levels (CODE_8089A0): VRAM $5E00 = 16 BG2 snow tiles, a new frame every odd
        # frame from weather 2; $5F00 = 16 BG1 snow tiles (BG1's empty cells, pattern (y&3)*4 + (x&3),
        # palette 2), a new frame every frame from weather 4; both blank below that.  BG1 is the 32X
        # canvas, so its snow goes on Genesis plane B instead (a second plane B map 'S', switched in
        # once the mountains have faded to the flat sky grey at 4, high priority = in front of BG2).
        # BG3 = the mountains/sky map at $7800 until the blizzard swaps BG3 to the snow map.  Ice Age's
        # VRAM holds blizzard frames in BG3 chars 0-63 (it starts at 5): the mountain chars come from
        # Snow Barrel Blast (same set).  The level data's BG2 map refers to the $5F00 copies of the
        # snow cells; the game's map uses $5E00 (SNES VRAM dumps).
        bg1, bg2 = lv['bgs'][0], lv['bgs'][1]
        t0 = ((0x5E00 * 2 - bg2['name'] * 2) & 0xFFFF) // 32
        t1 = ((0x5F00 * 2 - bg1['name'] * 2) & 0xFFFF) // 32
        frames = [L.rom.read(0xDEEE74 + f * 0x200, 0x200) for f in range(8)]
        anim = [(2, t0, 16, frames), (1, t1, 16, frames)]
        vram = bytearray(vram)
        if lv['entrance'] != 0x24:
            sb = [l for l in L.levels if l['entrance'] == 0x24][0]
            vsb, _ = build_vram(L, sb)
            vram[0xC000:0xC400] = vsb[0xC000:0xC400]
        n = 0x400 * (1 + (bg2['scs'] & 1)) * (1 + ((bg2['scs'] >> 1) & 1))
        for k in range(n):
            a = ((bg2['sc'] + k) * 2) & 0xFFFF
            w = vram[a] | vram[a + 1] << 8
            if t0 + 16 <= (w & 0x3FF) < t0 + 32:
                w -= 16
                vram[a], vram[a + 1] = w & 0xFF, w >> 8
        vram = bytes(vram)
        ents_override = {'B': [[vram[(0x7800 + ty * 32 + tx) * 2] | vram[(0x7800 + ty * 32 + tx) * 2 + 1] << 8
                                for tx in range(32)] for ty in range(32)],
                         'S': [[(t1 + (ty & 3) * 4 + (tx & 3)) | 2 << 10 for tx in range(4)] for ty in range(4)]}
        planes = [('A', 2), ('B', 3), ('S', 1)]
    elif lv['entrance'] == 0xA7 and any(l == 2 for _, l in planes):
        # weather >= 2 (CODE_8089A0): BG2 snow tiles at VRAM $5E00 re-streamed every odd frame from
        # DATA_DEEE74 + 0x200 * ((frame & 0xE) >> 1) (DATA_808BE8)
        bg2 = lv['bgs'][1]
        t0 = ((0x5E00 * 2 - bg2['name'] * 2) & 0xFFFF) // 32
        anim = (2, t0, 16, [L.rom.read(0xDEEE74 + f * 0x200, 0x200) for f in range(8)])
        # with the weather on, the BG2 map's snow cells use the animated tiles t0..t0+15 instead of
        # the static ones t0+16..t0+31 (matches the SNES VRAM dump)
        vram = bytearray(vram)
        n = 0x400 * (1 + (bg2['scs'] & 1)) * (1 + ((bg2['scs'] >> 1) & 1))
        for k in range(n):
            a = ((bg2['sc'] + k) * 2) & 0xFFFF
            w = vram[a] | vram[a + 1] << 8
            if t0 + 16 <= (w & 0x3FF) < t0 + 32:
                w -= 16
                vram[a], vram[a + 1] = w & 0xFF, w >> 8
    wx = None
    if lv['entrance'] in JUNGLE_WX:
        # darkness fades (Ropey: night -> day; Jungle Hijinxs / Orang-utan Gang: day -> night): the
        # palette lines are built from the day colours, CRAMs for both ends
        rainy = lv['entrance'] in RAIN_LEVELS
        cg_night, sky_night = (cg, sky_override) if rainy else jungle_night(L, cg_day)
        bg3_end = [0] * 224                       # BG3 haze gradient at the end (night: black)
        sunset = jungle_sunset(lv['entrance']) if not rainy else None
        if sunset:
            cg_night, sky_night, bg3_end = sunset
        night = {'cg': cg_night}
        if rainy:
            # lightning (CODE_80B096): colours 17-127 of DATA_B9A1DC as (r / 2, g / 2, min(2 b, 31))
            cg_flash = bytearray(cg_night)
            for i in range(17, 128):
                c = L.rom.u16(0xB9A1DC + i * 2)
                bb = ((c & 0x7C00) << 1) & 0xFFFF
                w = ((c >> 1) & 0x01EF) | (0x7C00 if bb & 0x8000 else bb)
                cg_flash[i * 2], cg_flash[i * 2 + 1] = w & 0xFF, w >> 8
            night = {'cgs': [cg_night, bytes(cg_flash)]}
        eo = None
        if rainy:
            # after the storm BG3 shows the distant treeline map at $2400 (= Jungle Hijinxs' BG3):
            # a second plane B map the 68000 switches in when the daylight fade starts
            planes = planes + [('S', 3)]
            eo = {'S': [[vram[(0x2400 + ty * 32 + tx) * 2] | vram[(0x2400 + ty * 32 + tx) * 2 + 1] << 8
                         for tx in range(32)] for ty in range(32)]}
        gtiles, gmaps, gcram_day, ganim = genesis_planes(vram, cg_day, lv, planes,
                                                         3 if t.get('fx') == 'jungle' else None, anim, eo,
                                                         day=night)
        if rainy:
            gmaps['S']['scroll'] = t['scroll'][3]
            gmaps['B'] = dict(map=[0] * 2048, w=64, h=32, shadow=gmaps['S']['shadow'], layer=3, raw=[0], rawwid=1)
        gcram_night = night['cram']
        gcram_flash = night['crams'][1] if rainy else None
        end_cg = cg_day if rainy else cg_night
        palE = [((end_cg[i * 2] | end_cg[i * 2 + 1] << 8) | 0x8000) if i % 16 else 0 for i in range(128)]
        skyE = list(sky_table(L, lv, cg_day)) if rainy else list(sky_night)
        gS, gE = (gcram_night, gcram_day) if rainy else (gcram_day, gcram_night)
        gcram = gS
        wx = dict(gN=list(gS), gD=list(gE), palD=palE, skyD=skyE, rain=rainy, bg3E=bg3_end, gF=gcram_flash)
    elif snow is not None:
        # palette keyframes per weather state (snes9x capture, build/weather.npz): the planes are built
        # from state 0 (full colour), every state's CRAM = per-entry mean of its member colours.  At 5
        # BG3 shows the blizzard instead, so plane B (colours 0-31) keeps state 4's flat grey.
        P = snow['pal']
        def cgb(k):
            q = [int(c) for c in P[k][:256]]
            if k == 5:
                q[:32] = [int(c) for c in P[4][:32]]
            return b''.join(bytes([c & 0xFF, c >> 8]) for c in q)
        keyk = {'cgs': [cgb(k) for k in range(6)]}
        # the blizzard (weather 5) goes on the Genesis planes: its 3 colours fixed at line 3, 1-3
        hcol = [gen_level(snes_rgb(int(P[5][v])), False) for v in (1, 2, 3)]
        keyk['reserve3'] = [tuple([c] * 7) for c in hcol]
        gtiles, gmaps, _, ganim = genesis_planes(vram, cgb(0), lv, planes, None, anim, ents_override, day=keyk)
        gcram = keyk['crams'][snow['start']]
        gmaps['H'] = heavy_snow_genesis(L, vram, len(gtiles))
        hflakes = gmaps['H']
        for g in ('A', 'B'):                     # low priority: the snow map 'S' goes in front of BG2
            gmaps[g]['map'] = [v & 0x7FFF for v in gmaps[g]['map']]
        gmaps['S']['map'] = [v | 0x8000 if v else v for v in gmaps['S']['map']]
        # (BG1 keeps its priority at 5: the trees on plane A must stay behind it, so the blizzard on
        # plane B goes behind BG1 too)
        wx = dict(gkeys=keyk['crams'], pkeys=[[(int(P[k][i]) | 0x8000) if i % 16 else 0
                                               for i in range(128)] for k in range(6)],
                  skeys=[int(P[k][0]) for k in range(6)])
        st = snow['start']
        pal[:128] = wx['pkeys'][st]
        sky = [wx['skeys'][st]] * 224
    elif lv['entrance'] in LIGHTS:
        li = LIGHTS[lv['entrance']]
        if 'src' in li:                          # palette computed at run time: its ROM source
            q = bytearray(512)
            for i in range(1, 128):              # (1-15: BG3's beam ramp, also BG1 palette 0's colours)
                w = L.rom.u16(li['src'] + i * 2)
                q[i * 2], q[i * 2 + 1] = w & 0xFF, w >> 8
            cg = bytes(q)
        cgk = light_cgs(cg, li['keys'])
        keyk = {'cgs': cgk}
        gtiles, gmaps, _, ganim = genesis_planes(vram, cgk[0], lv, planes, 3 if t.get('fx') == 'jungle' else None,
                                                 anim, day=keyk)
        gcram = keyk['crams'][0]
        wx = dict(gkeys=keyk['crams'], pkeys=[[((q[i * 2] | q[i * 2 + 1] << 8) | 0x8000) if i % 16 else 0
                                               for i in range(128)] for q in cgk],
                  skeys=[q[0] | q[1] << 8 for q in cgk])
        if 'beam' in li:
            # Squawks' beam: the 32X keeps the dark palette (the beam's pixels use the lit copy at 128 +),
            # the Genesis plane is shadowed everywhere (low priority) and highlighted in the beam
            used = {pal16 * 4 + v for pidx, pal16, hf in ts.bake if pidx != 'C' for b in ts.pool[pidx] for v in (b >> 4, b & 15) if v}
            hi = sorted(i for i in used if i >= 126)   # (125 -> 253, the HUD yellow: only tints the HUD)
            print('   beam: BG1 uses 32X colours %d-%d' % (min(used), max(used)))
            # (rooms: game mode only - lit colours 1-127, the dark ones mapped, sh/main.c bm_game_setup)
            assert not hi or lv.get('room'), 'beam: BG1 uses 32X colours %s (lit copies would hit the HUD colours)' % hi
            wx['lit'] = wx['pkeys'][1]
            wx['pkeys'] = [wx['pkeys'][0]] * 2
            wx['skeys'] = [wx['skeys'][0]] * 2
            for g in gmaps.values():
                g['map'] = [v & 0x7FFF for v in g['map']]
                g['shadow'] = True
            gmaps['L'] = beam_sprites(li['beam'][0], li['beam'][1], (len(gtiles) + 15) & ~15)
        pal[:128] = wx['pkeys'][0]
        sky = [wx['skeys'][0]] * 224
    else:
        gtiles, gmaps, gcram, ganim = genesis_planes(vram, cg, lv, planes, 3 if t.get('fx') == 'jungle' else None, anim,
                                                     normal=lv['entrance'] in LAMP_SPR)
    if comp:
        # plane A: opaque black cells over the side borders (x < 32, x >= 288), fixed scroll, plus the
        # HUD font (the 68000 draws the HUD on plane A for comp levels: tiles 2 + 2*char, 8x16)
        gtiles = [b'\0' * 32, b'\x11' * 32] + hud_font_tiles()
        bmap = [1 if (x < 4 or 36 <= x < 40) else 0 for y in range(32) for x in range(64)]
        gmaps = {'A': dict(map=bmap, h=32, layer=None, shadow=False, fixed=True)}
        gcram = [0] * 64
        gcram[2] = 0x0EEE
    # start position / bounds
    sx, sy = lv['start']
    if vert:
        xmin, xmax, ymin, ymax = 0, 2048 - 320, 0, gh * 32 - 224
    else:
        xmin, xmax = lv['x_min'], lv['x_max'] + 256 - 320
        if xmax < xmin:                         # single-screen arena: centred, 32-pixel margins
            xmin = xmax = lv['x_min'] - 32
        ymin, ymax = 0, 512 - 224
    if lv['entrance'] == 0x6D:                   # Slipslide: the glimmer as Genesis highlight sprites
        t0 = (len(gtiles) + 15) & ~15
        g_ = glimmer_genesis(L, lv, vram, t0)
        gtiles = gtiles + [b'\0' * 32] * (t0 - len(gtiles)) + g_['tiles']
        assert len(gtiles) < 0x500
        gmaps['G'] = g_
    lamp = None
    if lv['entrance'] in LAMP_SPR:
        lamp, cone = lamp_block(L, lv, bg1_opaque_fn(ts, grid, gw, gh, cx0, cy0), vram, cg)
        # (I18) S/H on: the cone on plane A in front of BG2 on plane B, both low priority = shadowed
        # (the cone colours at the shadow levels), except BG2's bright cells (the lanterns, normal
        # colours): high priority, in front of the cone - its dots would darken them (a dot replaces
        # the pixel: the light could only be added where the scene is dark)
        for g in gmaps.values():
            assert not any(v and (v >> 13) & 3 == 3 for v in g['map']), 'lamps: palette line 3 in use'
        assert not any(v & 0x8000 for k, g in gmaps.items() if k != 'B' for v in g['map'])
        print('   lamps: %d of %d BG2 cells bright (in front of the cone)' % (
            sum(1 for v in gmaps['B']['map'] if v & 0x8000), len(gmaps['B']['map'])))
        assert not any(gcram[48 + 1:48 + 11]), 'lamps: CRAM line 3 colours in use'
        t0 = len(gtiles)
        assert t0 + len(cone['tiles']) < 0x500
        gtiles = gtiles + cone['tiles']
        cone['tile0'] = t0
        cone['map'] = [(t0 + i - 1) | 3 << 13 if i else 0 for i in cone['map']]
        gmaps['A'] = dict(map=[0] * (64 * 64), w=64, h=64, shadow=False, layer=None, fixed=True)
        gmaps['C'] = cone
        assert all(len(g.get('map', [])) for g in gmaps.values())
    lv['_gmaps'] = gmaps
    return dict(lv=lv, type=t, ts=ts, grid=grid, gw=gw, gh=gh, cx0=cx0, cy0=cy0, overlays=overlays,
                pal=pal, sky=sky, gtiles=gtiles, gmaps=gmaps, gcram=gcram, ganim=ganim,
                xmin=xmin, xmax=xmax, ymin=ymin, ymax=ymax, start=(sx, sy), vram=vram, cg=cg, perf=perf,
                glow=lamp if lamp is not None else
                rain_block(L, lv, vram, cg, wx, wx['rain']) if lv['entrance'] in JUNGLE_WX else
                snow_block(L, lv, snow, wx) if snow is not None else
                lights_block(L, lv, wx) if lv['entrance'] in LIGHTS else
                millstone_block(L, lv, vram) if t.get('name') == 'temple' and lv['entrance'] in (0x42, 0x43) else None)


# ------------------------------------------------------------------ scroll formula (shared with C)
def scroll_val(cam, mode, shift, sub=0, clamp=None):
    v = cam - sub
    if clamp is not None and v > clamp:
        v = clamp
    if mode == 0:
        return v >> shift
    if mode == 1:
        return v + (v >> 1)
    if mode == 2:
        return v << 1
    if mode == 4:
        return (v >> shift) & 0xFF
    if mode == 5:
        return (v << 1) + shift
    return shift


# ------------------------------------------------------------------ preview compositor
def c32(w):
    return ((w & 31) * 255 // 31, ((w >> 5) & 31) * 255 // 31, ((w >> 10) & 31) * 255 // 31)


def cgen(w, shadow):
    return tuple(GEN_LEVELS[((w >> (1 + i * 4)) & 7) * (1 if shadow else 2)] for i in range(3))


def preview(lvl, camx, camy, path):
    if any(o.get('role') for o in lvl['overlays']):
        return                            # comp levels: no software preview yet
    from PIL import Image
    ts = lvl['ts']
    img = Image.new('RGB', (320, 224))
    pix = img.load()
    bake_px = {}

    def bpx(be, x, y):
        b = be & 0x7FFF
        if b not in bake_px:
            pidx, pal16, hf = ts.plain(b)
            t = ts.pool[pidx]
            px = []
            for i in range(64):
                byte = t[i >> 1]
                px.append(byte >> 4 if i % 2 == 0 else byte & 15)
            rows = [px[r * 8:r * 8 + 8] for r in range(8)]
            if hf:
                rows = [r[::-1] for r in rows]
            bake_px[b] = (rows, pal16)
        rows, pal16 = bake_px[b]
        if be & 0x8000:
            y = 7 - y
        v = rows[y][x]
        return (pal16 * 4 + v) if v else 0

    gm = lvl['gmaps']
    for y in range(224):
        for x in range(320):
            # overlays (front)
            done = False
            for ov in lvl['overlays']:
                sxm, sxs, sym, sys_, sysub, syc = ov['scroll']
                ox = (scroll_val(camx, sxm, sxs) + x) % (ov['w'] * 8)
                oy = (scroll_val(camy, sym, sys_, sysub, syc) + y + 1) % (ov['h'] * 8)
                if ov.get('anim'):
                    ox = (x) % (ov['w'] * 8)
                    oy = (y + 1) % (ov['h'] * 8)
                be = ov['map'][(oy >> 3) * ov['w'] + (ox >> 3)]
                if be != 0x7FFF and (be & 0x4000):
                    be = ov['anim'][be & 0x3FFF]
                if be != 0x7FFF:
                    v = bpx(be, ox & 7, oy & 7)
                    if v:
                        pix[x, y] = c32(lvl['pal'][v])
                        done = True
                        break
            if done:
                continue
            lx, ly = camx + x - lvl['cx0'] * 32, camy + y + 1 - lvl['cy0'] * 32
            tx, ty = lx >> 3, ly >> 3
            cx, cy = tx >> 2, ty >> 2
            if 0 <= cx < lvl['gw'] and 0 <= cy < lvl['gh']:
                vc = lvl['grid'][cy * lvl['gw'] + cx]
                be = ts.vchunks[vc][(ty & 3) * 4 + (tx & 3)]
                v = bpx(be, lx & 7, ly & 7)
                if v:
                    pix[x, y] = c32(lvl['pal'][v])
                    continue
            # genesis planes A then B
            drawn = False
            for letter in ('A', 'B'):
                if letter not in gm:
                    continue
                g = gm[letter]
                if g.get('layer') not in lvl['type']['scroll']:
                    continue                      # (a fixed plane: borders, lamp cones)
                sc = lvl['type']['scroll'][g['layer']]
                gx = (scroll_val(camx, sc[0], sc[1]) + x) % (g['w'] * 8)
                gy = (scroll_val(camy, sc[2], sc[3], sc[4], sc[5]) + y + 1) % (g['h'] * 8)
                ent = g['map'][(gy >> 3) * g['w'] + (gx >> 3)]
                tb = lvl['gtiles'][ent & 0x7FF]
                ttx, tty = gx & 7, gy & 7
                if ent & 0x800:
                    ttx = 7 - ttx
                if ent & 0x1000:
                    tty = 7 - tty
                byte = tb[tty * 4 + ttx // 2]
                ci = byte >> 4 if ttx % 2 == 0 else byte & 15
                if ci:
                    pix[x, y] = cgen(lvl['gcram'][((ent >> 13) & 3) * 16 + ci], g['shadow'])
                    drawn = True
                    break
            if not drawn:
                pix[x, y] = c32(lvl['sky'][y])
    img.save(path)


# ------------------------------------------------------------------ pack writer
class Pack:
    def __init__(self):
        self.buf = bytearray()

    def align(self, a=4):
        while len(self.buf) % a:
            self.buf.append(0)

    def add(self, data, align=4):
        self.align(align)
        off = len(self.buf)
        self.buf += data
        return off


def main():
    rom = Rom(sys.argv[1])
    rom.check()
    out = sys.argv[2]
    os.makedirs(out, exist_ok=True)
    L = Levels(rom)
    only = [int(x, 16) for x in sys.argv[3:]] if len(sys.argv) > 3 else None
    tilesets = {}
    built = []
    worlds_in = set(lv['world'] for lv in L.levels if not only or lv['entrance'] in only)
    for lv in L.levels:
        if only and lv['entrance'] not in only and not (lv.get('room') and lv['world'] in worlds_in):
            continue
        if lv['nmi'] not in TYPES:
            print('skip', lv['name'], lv['nmi'])
            continue
        b = build_level(L, lv, tilesets)
        b['L'] = L
        built.append(b)
        print('%02X %-22s grid %dx%d vchunks(ts) %d bake(ts) %d pool(ts) %d gtiles %d ov %d' % (
            lv['entrance'], lv['name'], b['gw'], b['gh'], len(b['ts'].vchunks), len(b['ts'].bake),
            len(b['ts'].pool), len(b['gtiles']), len(b['overlays'])))
    if '--preview' in os.environ.get('DKC_FLAGS', '') or only:
        os.makedirs(os.path.join(out, 'preview'), exist_ok=True)
        for b in built:
            sx, sy = b['start']
            camx = max(b['xmin'], min(b['xmax'], sx - 64))
            camy = max(b['ymin'], min(b['ymax'], sy))
            preview(b, camx, camy, os.path.join(out, 'preview', '%02X.png' % b['lv']['entrance']))
    json.dump([dict(e=b['lv']['entrance'], name=b['lv']['name']) for b in built], open(os.path.join(out, 'built.json'), 'w'))
    mx = dict(slots=0, vch=0, bake=0)
    for b in built:
        used = set(b['grid'])
        slots = set(b['ts'].vchunks[v][k] & 0x7FFF for v in used for k in range(16))
        for ov in b['overlays']:
            if ov.get('anim'):
                pass                      # drawn from the flake lists, no baked slots
            elif ov.get('role') == 'light':
                slots |= set(('light', i) for i in range(len(ov['ltiles'])))
            else:
                slots |= set(e & 0x7FFF for e in ov['map'] if e != 0x7FFF)
        mx['slots'] = max(mx['slots'], len(slots))
        mx['vch'] = max(mx['vch'], len(b['ts'].vchunks))
        mx['bake'] = max(mx['bake'], len(b['ts'].bake))
    print('max per-level slots %(slots)d, tileset vchunks %(vch)d, tileset bake %(bake)d' % mx)
    open(os.path.join(out, 'pack.h'), 'w').write('#define PACK_MAX_SLOTS %d\n#define PACK_MAX_VCH %d\n#define PACK_MAX_BAKE %d\n' % (mx['slots'], mx['vch'], mx['bake']))
    write_pack(built, tilesets, L, out)
    return built, tilesets



# ================================================================== pack v2
PACK_ROM = 0x040000     # ROM offset where the pack is placed (SH-2: 0x02040000)
LEVEL_REC = 140
WORLD_REC = 64


def font_table():
    from PIL import Image, ImageDraw, ImageFont
    f = ImageFont.load_default()
    out = bytearray()
    for i in range(96):
        ch = chr(32 + i)
        img = Image.new('L', (12, 12), 0)
        ImageDraw.Draw(img).text((0, -2), ch, font=f, fill=255)
        px = img.load()
        rows = [sum(1 << (7 - x) for x in range(8) if px[x, y] > 110) for y in range(10)]
        w = 0
        for x in range(8):
            if any(px[x, y] > 110 for y in range(10)):
                w = x + 1
        if ch == ' ':
            w = 3
        out.append(min(8, w + 1))
        out += bytes(rows)
    return bytes(out)


def map_image(L, m):
    """world map BG1 -> 256x224 8bpp image + 128 colour palette"""
    m = dict(m)
    m['layout'] = None
    vram, cg = build_vram(L, m)
    b = m['bgs'][0]
    img = bytearray(256 * 224)
    for ty in range(28):
        for tx in range(32):
            a = map_addr(b['sc'], 0, tx, ty) * 2
            e = vram[a] | vram[a + 1] << 8
            t, p = e & 0x3FF, (e >> 10) & 7
            off = (b['name'] * 2 + t * 32) & 0xFFFF
            pp = decode_4bpp_tile(bytes(vram[(off + i) & 0xFFFF] for i in range(32)), 0)
            for y in range(8):
                for x in range(8):
                    xx = 7 - x if e & 0x4000 else x
                    yy = 7 - y if e & 0x8000 else y
                    v = pp[yy * 8 + xx]
                    img[(ty * 8 + y) * 256 + tx * 8 + x] = p * 16 + v if v else 0
    pal = [(cg[i * 2] | cg[i * 2 + 1] << 8) | 0x8000 for i in range(128)]
    return bytes(img), pal


# the game's map screens by index ($02 while a map is shown, CODE_80E1F8 / DATA_80E176): graphics,
# BG1 map (32x28 at VRAM $7C00, chars at $1000 words) and palette, all uncompressed in the ROM
MAP_GFX = [0xC116F1, 0xC51012, 0xC61757, 0xC67CF7, 0xCC8BB4, 0xF70000, 0xF41558, 0xF20700, 0xFF0701,
           0xD10701, 0xD16DC3, 0xCA0701, 0xC828E4]
MAP_TM = [0xC10FF0, 0xC50912, 0xC61057, 0xC675F7, 0xCC84B4, 0xC40000, 0xF40E57, 0xF20000, 0xFF0000,
          0xD10000, 0xD166C2, 0xCA0000, 0xC821E4]
MAP_PAL = [0xB9BE03, 0xB9C103, 0xB9C003, 0xB9C523, 0xB9C423, 0xB9C323, 0xB9C223, 0xB9CC83, 0xB9CD63,
           0xB9D143, 0xB9D243, 0xB9CF43, 0xB9D043]
# map screens of each world (Kongo Jungle 1, 2, Monkey Mines 1, 2, Vine Valley, Gorilla Glacier,
# Kremkroc, Chimp Caverns) in maps.json order
WORLD_MAPS = {'Kongo Jungle': (1, 2), 'Monkey Mines': (3, 4), 'Vine Valley': (11, 12), 'Gorilla Glacier': (5, 6),
              'Kremkroc Industries': (7, 8), 'Chimp Caverns': (9, 10)}


def map_screen_image(L, mi):
    """map screen mi -> 256x224 8bpp image + 128 colour palette (straight from the ROM tables)"""
    rom = L.rom
    tm = rom.read(MAP_TM[mi], 1792)
    gfx = MAP_GFX[mi]
    img = bytearray(256 * 224)
    cache = {}
    for ty in range(28):
        for tx in range(32):
            e = tm[(ty * 32 + tx) * 2] | tm[(ty * 32 + tx) * 2 + 1] << 8
            t, p = e & 0x3FF, (e >> 10) & 7
            if t not in cache:
                cache[t] = decode_4bpp_tile(rom.read(gfx + t * 32, 32), 0)
            pp = cache[t]
            for y in range(8):
                for x in range(8):
                    xx = 7 - x if e & 0x4000 else x
                    yy = 7 - y if e & 0x8000 else y
                    v = pp[yy * 8 + xx]
                    img[(ty * 8 + y) * 256 + tx * 8 + x] = p * 16 + v if v else 0
    pal = [rom.u16(MAP_PAL[mi] + i * 2) | 0x8000 for i in range(128)]
    return bytes(img), pal


# the cabins (screens the game shows on a map: entrance $3E = EE-F3 Cranky, F4-F9 Funky, FA-FF Candy):
# BG1 picture (+ Cranky's BG2 sky) from ROM assets (recipes: snes9x VRAM dumps matched to the
# disassembly's files), the dialogue on BG3 (ASCII tile numbers, font GFX_Sprite_AreaNameFont at
# char $20) is drawn at run time from the game's VRAM writes
_CAB_BGS = [{'sc': 31744, 'name': 4096, 'scs': 0, 'h': 0, 'v': 255, 'bpp': 4},
            {'sc': 30720, 'name': 0, 'scs': 0, 'h': 0, 'v': 0, 'bpp': 4}]
SCREENS = [
    dict(name='Cranky', bgs=[{'sc': 31744, 'name': 12288, 'scs': 0, 'h': 0, 'v': 255, 'bpp': 4},
                             {'sc': 30720, 'name': 4096, 'scs': 0, 'h': 76, 'v': 255, 'bpp': 4}], layers=(1, 0),
         vram_runs=[(8192, 12928, 'GFX_Layer2_IntroScreen', 0), (24576, 23744, 'GFX_Layer1_CrankysCabin', 0),
                    (61440, 576, 'TM_Layer2_IntroSky', 0), (63552, 1728, 'TM_Layer1_CrankysCabin', 0)],
         cgram_runs=[(4, 252, 'PAL_FGBG_CrankysCabin', 4)], txh=-4),
    dict(name='Funky', bgs=_CAB_BGS, layers=(0,), vram_runs=[(8192, 23968, 'GFX_Layer1_FunkysFlights', 0),
         (63488, 1792, 'TM_Layer1_FunkysFlights', 0)], cgram_runs=[(4, 252, 'PAL_Layer1_FunkysFlights', 4)], txh=0),
    dict(name='Candy', bgs=_CAB_BGS, layers=(0,), vram_runs=[(8192, 24544, 'GFX_Layer1_CandysSavePoint', 0),
         (63488, 1792, 'TM_Layer1_CandysSavePoint', 0)], cgram_runs=[(4, 252, 'PAL_Layer1_CandysSavePoint', 4)], txh=0),
]


def screen_image(L, m):
    """a cabin screen: its BG layers (back to front, with their scroll) -> 256x224 8bpp + 128 colours"""
    m = dict(m, layout=None)
    vram, cg = build_vram(L, m)
    img = bytearray(256 * 224)
    tm_start = {}                                # map words before a layer's map upload: empty
    for pos, ln, name, off in m['vram_runs']:
        if name.startswith('TM_'): tm_start[pos >> 11] = pos
    for li in m['layers']:
        b = m['bgs'][li]
        for y in range(224):
            my = (y + b['v'] + 1) & 255
            for x in range(256):
                mx = (x + b['h']) & 255
                a = map_addr(b['sc'], 0, mx >> 3, my >> 3) * 2
                if a < tm_start.get(a >> 11, 0): continue
                e = vram[a] | vram[a + 1] << 8
                t, p = e & 0x3FF, (e >> 10) & 7
                px, py = mx & 7, my & 7
                if e & 0x4000: px = 7 - px
                if e & 0x8000: py = 7 - py
                off = (b['name'] * 2 + t * 32) & 0xFFFF
                row = vram[off + py * 2], vram[off + py * 2 + 1], vram[off + 16 + py * 2], vram[off + 17 + py * 2]
                sh = 7 - px
                v = (row[0] >> sh & 1) | (row[1] >> sh & 1) << 1 | (row[2] >> sh & 1) << 2 | (row[3] >> sh & 1) << 3
                if v: img[y * 256 + x] = p * 16 + v
    pal = [(cg[i * 2] | cg[i * 2 + 1] << 8) | 0x8000 for i in range(128)]
    return bytes(img), pal


# (I11) front-end screens (NMI routine) whose BG2 the Genesis shows as plane B: Cranky's speech / cabin -
# the clouds, behind everything (priority 0 tiles, one palette), only scroll sideways: the 32X leaves
# their pixels transparent and never redraws a line for them (they cost 1/4 frame a picture and still
# crawled behind)
FE_GPLANE = {0xEEA3}


def fe_bg2_plane(vr, cg, rg):
    """the 68000's plane block (md_block layout: plane B only, scroll = BG2's H / V offset as posted by
    the 32X) for a front-end screen's BG2 - None if the layer does not qualify"""
    if (rg[0x2105] & 7) != 1 or not (rg[0x212C] & 2) or (rg[0x212D] & 2):
        return None
    sc = rg[0x2108]
    bg = dict(bpp=4, scs=sc & 3, sc=(sc & 0xFC) << 8, name=(rg[0x210B] >> 4) << 12)
    wid, hgt = (64 if sc & 1 else 32), (64 if sc & 2 else 32)
    for ty in range(hgt):
        for tx in range(wid):
            a = map_addr(bg['sc'], bg['scs'], tx, ty) * 2
            e = vr[a] | vr[a + 1] << 8
            t = (bg['name'] * 2 + (e & 0x3FF) * 32) & 0xFFFF
            if any(vr[(t + i) & 0xFFFF] for i in range(32)) and (e & 0x2000):
                return None                     # (a priority 1 tile: in front of BG1's low tiles)
    lv = dict(regs=dict(CGADSUB=rg[0x2131], CGWSEL=rg[0x2130], fixed=[0, 0, 0]), bgs=[None, bg, None, None])
    gtiles, gmaps, cram, _ = genesis_planes(vr, cg, lv, [('B', 2)])
    cram = list(cram)
    cram[0] = gen_level(snes_rgb(cg[0] | cg[1] << 8), False)   # the backdrop (Genesis colour 0)
    g = gmaps['B']
    flags = 2 | (4 if g['h'] == 64 else 0)
    hdr = bytearray()
    hdr += be16(len(gtiles)) + be16(flags)
    for v in [0, 0, 0, 0, 0, 0xFFFF] + [0, 0, 0, 0, 0, 0xFFFF]:    # (A unused; B: x = cam, y = cam)
        hdr += be16(v)
    hdr += be16(0) + be16(0)
    for c in cram:
        hdr += be16(c)
    hdr += bytes(2 * 224)
    assert len(hdr) == 608
    for tb in gtiles:
        hdr += tb
    for ty in range(g['h']):
        for tx in range(64):
            hdr += be16(g['map'][ty * 64 + tx])
    return bytes(hdr)


def md_block(b, pf=None):
    t = b['type']
    gm = pf['gmaps'] if pf else b['gmaps']
    tall = any(g.get('h') == 64 for g in gm.values())
    flags = (1 if 'A' in gm else 0) | (2 if 'B' in gm else 0) | (4 if tall else 0) | (8 if 'S' in gm else 0) | \
        (16 if 'H' in gm else 0) | (32 if 'L' in gm else 0) | (64 if 'C' in gm else 0) | (128 if 'G' in gm else 0)
    h = 64 if tall else 32

    def sc(letter):
        if letter not in gm:
            return [0] * 6
        if gm[letter].get('fixed'):
            return [3, 0, 3, 0, 0, 0xFFFF]
        s = t['scroll'][gm[letter]['layer']]
        return [s[0], s[1], s[2], s[3], s[4], 0xFFFF if s[5] is None else s[5]]
    fx = 1 if t.get('fx') == 'jungle' else 3 if t.get('fx') == 'water' else 6 if t.get('fx') == 'galleon' else 0
    if 'A' in gm and gm['A'].get('fixed'):
        fx = 2                                   # comp level: 68000 draws the HUD on plane A
    if pf:
        # performance variant: plane A = BG1 streamed by the 68000; 4 = plane B underwater perspective
        fx = 4 if t.get('fx') == 'water' else 5
    bg3col = [0] * 224
    if fx == 1:
        rom = b['L'].rom
        data = rom.read(0xB998EA, 0xA0)
        cols = []
        i = 0
        while i < len(data) and len(cols) < 224:
            n = data[i]
            if n == 0:
                break
            cols += [data[i + 3] | data[i + 4] << 8] * (n & 0x7F)
            i += 5
        while len(cols) < 224:
            cols.append(cols[-1])
        # BG3 is plane B: colour 1 of its palette line, shadow aware
        sh = gm['B']['shadow'] if 'B' in gm else True
        bg3col = [gen_level(apply_colormath(snes_rgb(c), b['lv']['regs'], 3), sh) for c in cols[:224]]
    gtiles = pf['gtiles'] if pf else b['gtiles']
    cram = pf['gcram'] if pf else b['gcram']
    hdr = bytearray()
    hdr += be16(len(gtiles)) + be16(flags)
    for v in sc('A') + sc('B'):
        hdr += be16(v)
    hdr += be16(fx) + be16(len(b.get('ganim') or []))
    for c in cram:
        hdr += be16(c)
    for c in bg3col:
        hdr += be16(c)
    assert len(hdr) == 608
    for tb in gtiles:
        hdr += tb
    for letter in ('A', 'B'):
        if letter in gm:
            g = gm[letter]
            for ty in range(h):
                for tx in range(64):
                    hdr += be16(g['map'][(ty % g['h']) * 64 + tx])
    # animated tiles: u16 genesis tile (| 0x8000: second group), 8 frames x 32 bytes
    for tile, frames in (b.get('ganim') or [] if not pf else []):
        hdr += be16(tile) + b''.join(frames)
    if 'S' in gm:
        # weather snow: second plane B map (64 x 32, at VRAM $8000) shown from weather 4 with BG1's
        # scroll (u16 scroll params[6], map)
        s_ = gm['S'].get('scroll')
        hdr += b''.join(be16(v) for v in ((s_[0], s_[1], s_[2], s_[3], s_[4], 0xFFFF if s_[5] is None else s_[5])
                                          if s_ else (0, 0, 0, 0, 0, 0xFFFF)))
        hdr += b''.join(be16(v) for v in gm['S']['map'][:64 * 32])
    if 'H' in gm:
        # weather snow: the blizzard (u16 first tile, 0, sway[64], map 64 x 32, tiles 8 frames x 64 x 32)
        h = gm['H']
        hdr += be16(h['hb']) + be16(0) + bytes(h['sway'])
        hdr += b''.join(be16(v) for v in h['map'])
        hdr += h['frames']
    if 'C' in gm:
        # lamp cones on plane A: u16 nlamps, pivot row, 0, 0, (cell x, cell y)[n], map[28][22] (plane
        # words), colours[4][10] (flicker d x level), swing: the game's sine DATA_808BA8[64] + 62 x 0
        c_ = gm['C']
        hdr += be16(len(c_['lamps'])) + be16(c_['pivot']) + be16(0) + be16(0)
        hdr += b''.join(be16(x) + be16(y) for x, y in c_['lamps'])
        hdr += b''.join(be16(v) for v in c_['map'])
        hdr += b''.join(be16(v) for v in c_['cols'])
        hdr += b''.join(be16(v & 0xFFFF) for v in c_['swing'])
    if 'G' in gm:
        # Slipslide glimmer: u16 nblocks, 0, 0, 0, (map x, map y, first tile)[n] (32 x 32 sprites)
        g_ = gm['G']
        hdr += be16(len(g_['blocks'])) + be16(0) * 3
        hdr += b''.join(be16(x) + be16(y) + be16(t_) for x, y, t_ in g_['blocks'])
    if 'L' in gm:
        # Squawks' beam: u16 nsprites, ntiles, first tile, 0, SAT entries[n][4] (y at bob 0), tiles
        L_ = gm['L']
        hdr += be16(len(L_['sat'])) + be16(len(L_['tiles'])) + be16(L_['sat'][0][2] & 0x7FF if L_['sat'] else 0) + be16(0)
        hdr += b''.join(be16(v) for e in L_['sat'] for v in e)
        hdr += b''.join(L_['tiles'])
    if pf:
        # streamed plane A: u16 gw, gh, ox0, oy0, nvch, pad, grid[gw*gh], vch[nvch*16]
        hdr += be16(pf['gw']) + be16(pf['gh']) + be16(pf['ox0']) + be16(pf['oy0']) + be16(len(pf['vch']) // 16) + be16(0)
        hdr += b''.join(be16(v) for v in pf['grid'])
        hdr += b''.join(be16(v) for v in pf['vch'])
    return bytes(hdr)


BANK_WIN = 0x380000      # ROM window 7 (SSF2): the current level bank, 512 KB
BANK_SIZE = 0x80000
BANK_OFF = BANK_WIN - PACK_ROM   # pack offset of the window


class BankPack(Pack):
    """a 512 KB ROM bank shown in window 7: offsets returned are pack offsets (BANK_OFF + local)"""
    def __init__(self, idx):
        Pack.__init__(self)
        self.idx = idx
        self.ts = {}

    def add(self, data, align=4):
        return BANK_OFF + Pack.add(self, data, align)


def emit_level(p, b, rom_base):
    """level b's data into pack p (Pack or BankPack); -> (record fields...) pack offsets"""
    if b['ts'].key not in getattr(p, 'ts', {}):
        ts = b['ts']
        pool = b''.join(ts.pool)
        bake = b''.join(be32(ts.bake_word(b)) for b in ts.bake)
        vch = b''.join(b''.join(be16(e) for e in v) for v in ts.vchunks)
        o_pool, o_bake, o_vch = p.add(pool), p.add(bake), p.add(vch)
        o_vprio = p.add(b''.join(be16(m) for m in ts.vprio)) if any(ts.vprio) else 0
        rec = (be32(len(ts.pool)) + be32(o_pool) + be32(len(ts.bake)) + be32(o_bake) + be32(len(ts.vchunks)) + be32(o_vch) +
               be32(o_vprio))
        p.ts[ts.key] = p.add(rec)
    o_ts = p.ts[b['ts'].key]
    o_grid = p.add(b''.join(be16(v) for v in b['grid']))
    o_pal = p.add(b''.join(be16(c) for c in b['pal']))
    o_sky = p.add(b''.join(be16(c) for c in b['sky']))
    ovs = []
    for ov in b['overlays'][:2]:
        extra = b''
        if ov.get('role') == 'base':
            extra = bytes(ov['remap'])
        elif ov.get('role') == 'light':
            extra = be16(len(ov['ltiles'])) + be16(1 if ov.get('wave') else 0) + b''.join(ov['ltiles'])
            if ov.get('wave'):
                # sway[64], u16 S, u16 K, u16 qcol[S], u8 vrep2[K]
                extra += ov['wave'] + be16(len(ov['qcol'])) + be16(len(ov['vrep2']))
                extra += b''.join(be16(c) for c in ov['qcol']) + bytes(ov['vrep2'])
                extra += b'\0' * (-len(extra) % 4)
        elif ov.get('anim'):
            # after the map: sway[64], wobble[32], u32 frame block offsets[8] (from map start),
            # then per frame: u16 row start[257] (pair index) + (x, colour) pairs
            nmap = len(ov['map']) * 2
            head = bytes(ov['sway']) + bytes(w & 0xFF for w in ov['wobble'])
            blocks = []
            for rows in ov['flakes']:
                idx, pairs, n = [], bytearray(), 0
                for row in rows:
                    idx.append(n)
                    for x, c in row:
                        pairs += bytes([x, c])
                        n += 1
                idx.append(n)
                blk = b''.join(be16(v) for v in idx) + bytes(pairs)
                blk += b'\0' * (-len(blk) % 4)
                blocks.append(blk)
            offs, o = [], nmap + len(head) + 4 * len(blocks)
            for blk in blocks:
                offs.append(o)
                o += len(blk)
            extra = head + b''.join(be32(v) for v in offs) + b''.join(blocks)
        o_map = p.add(b''.join(be16(v) for v in ov['map']) + extra)
        sc = ov['scroll']
        ovs.append(be16(ov['w']) + be16(ov['h']) + bytes([sc[0], sc[1] & 0xFF, sc[2], sc[3] & 0xFF]) + be16(sc[4]) +
                   be16(0xFFFF if sc[5] is None else sc[5]) + be32(o_map))

    def mb_block(data):
        # keep a 68K-read block inside one 1 MB ROM bank (the 68K's 0x900000 window)
        p.align(4)
        start = rom_base + len(p.buf)
        if (start >> 20) != ((start + len(data)) >> 20):
            p.buf += b'\0' * (((start >> 20) + 1 << 20) - start)
        return p.add(data)
    md = md_block(b)
    o_md = mb_block(md)
    o_md2 = o_fog = 0
    if b.get('glow'):
        o_fog = mb_block(b['glow'])       # (the 68000 reads the weather blocks' CRAM keyframes)
    if b.get('perf'):
        pf, fog = b['perf']
        o_md2 = mb_block(md_block(b, pf))
        o_fog = p.add(fog)
    o_snap = 0
    sp = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'game', 'snap_%02X.bin' % b['lv']['entrance'])
    if os.path.exists(sp):
        o_snap = p.add(open(sp, 'rb').read())
    return dict(o_ts=o_ts, o_grid=o_grid, o_pal=o_pal, o_sky=o_sky, ovs=ovs, o_md=o_md, md_len=len(md),
                o_md2=o_md2, o_fog=o_fog, o_snap=o_snap)


def write_pack(built, tilesets, L, out):
    maps = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'maps.json')))
    p = Pack()
    nlev, nworld = len(built), len(maps)
    p.add(b'\0' * 52)
    off_levels = p.add(b'\0' * (LEVEL_REC * nlev))
    off_worlds = p.add(b'\0' * (WORLD_REC * nworld))
    off_font = p.add(font_table())
    banks = [BankPack(0)]

    def into_bank(fn):
        """run fn(bank) in the bank where it adds the least (shared tile sets) and still fits, the
        fullest of those on a tie; a new bank if none (best fit: the ROM must stay <= 15 MB for the
        Mega EverDrive Pro)"""
        best = None
        for bk in banks:
            n0, ts0 = len(bk.buf), dict(bk.ts)
            fn(bk)
            grow, fits = len(bk.buf) - n0, len(bk.buf) <= BANK_SIZE
            del bk.buf[n0:]
            bk.ts = ts0
            if fits and (best is None or (grow, -n0) < best[1]):
                best = (bk, (grow, -n0))
        if best is None:
            bk = BankPack(len(banks))
            banks.append(bk)
        else:
            bk = best[0]
        r = fn(bk)
        assert len(bk.buf) <= BANK_SIZE, 'item over a ROM bank'
        return bk.idx, r

    world_of = {}
    for wi, m in enumerate(maps):
        world_of[m['name']] = wi
    world_levels = [[] for _ in maps]
    # (H1) bank packing order: tile-set groups by total size, largest first, each group's levels
    # largest first (best fit decreasing; levels sharing a tile set land in its bank when they fit)
    def alone(b):
        t = BankPack(0)
        emit_level(t, b, BANK_WIN)
        return len(t.buf)
    sz = [alone(b) for b in built]
    grp = {}
    for li, b in enumerate(built):
        grp.setdefault(b['ts'].key, []).append(li)
    order = []
    for k, lis in sorted(grp.items(), key=lambda kv: -sum(sz[i] for i in kv[1])):
        order += sorted(lis, key=lambda i: -sz[i])
    placed = {}
    for li in order:
        b = built[li]
        placed[li] = into_bank(lambda bk: emit_level(bk, b, BANK_WIN))
    for li, b in enumerate(built):
        lv = b['lv']
        bank, f = placed[li]
        name = lv['name'].encode('ascii')[:27].ljust(28, b'\0')
        wi = world_of.get(lv['world'], 0xFFFF)
        if wi != 0xFFFF and not lv.get('room'):
            world_levels[wi].append(li)
        rec = be16(lv['entrance']) + be16(wi) + name
        rec += be16(b['gw']) + be16(b['gh']) + be16(b['cx0']) + be16(b['cy0'])
        for v in (b['xmin'], b['xmax'], b['ymin'], b['ymax'], b['start'][0], b['start'][1]):
            rec += be32(v)
        rec += be32(f['o_ts']) + be32(f['o_grid']) + be32(f['o_pal']) + be32(f['o_sky'])
        ovs = f['ovs']
        rec += be32(len(ovs))
        for k in range(2):
            rec += ovs[k] if k < len(ovs) else b'\0' * 16
        lflags = (1 if lv['stream'] in (0x8168, 0x831B) else 0) | (2 if b['type'].get('fx') == 'water' else 0)
        if any(o.get('role') == 'light' for o in b['overlays']):
            lflags |= 4
        if any(o.get('wave') for o in b['overlays']):
            lflags |= 8
        if lv['entrance'] in COMP:
            lflags |= (COMP[lv['entrance']]['K'] * COMP[lv['entrance']]['S']) << 8   # unlit bank U
        if b.get('glow'):
            lflags |= 0x10
        lflags |= (7 + bank) << 24        # physical ROM bank shown in window 7 (SSF2 register value)
        rec += be32(f['o_md']) + be32(f['md_len']) + be32(lflags) + be32(f['o_md2']) + be32(f['o_fog'])
        rec += be32(f['o_snap'])
        assert len(rec) == LEVEL_REC, len(rec)
        p.buf[off_levels + li * LEVEL_REC: off_levels + (li + 1) * LEVEL_REC] = rec
    # galleon (no world) goes into the last world's list
    for li, b in enumerate(built):
        if b['lv']['world'] not in world_of:
            world_levels[-1].append(li)
    shared_map = None                     # (worlds without levels in this build share one picture: ROM space)
    for wi, m in enumerate(maps):
        if world_levels[wi] or shared_map is None:
            img, pal = map_image(L, m)
            bank, (o_img, o_pal) = into_bank(lambda bk: (bk.add(img), bk.add(b''.join(be16(c) for c in pal))))
            if shared_map is None: shared_map = (o_img, o_pal, bank)
        else:
            o_img, o_pal, bank = shared_map
        rec = m['name'].encode()[:27].ljust(28, b'\0') + be32(len(world_levels[wi]))
        idx = world_levels[wi][:8] + [0] * (8 - len(world_levels[wi][:8]))
        rec += b''.join(be16(i) for i in idx) + be32(o_img) + be32(o_pal) + bytes([7 + bank]) + b'\0' * 7
        assert len(rec) == WORLD_REC
        p.buf[off_worlds + wi * WORLD_REC: off_worlds + (wi + 1) * WORLD_REC] = rec
    # the game's map screens (13 x u32 image, u32 palette, u8 bank, 3 pad; image 0 = not in this
    # build): the world map and both screens of every world with levels in the build
    want = {0}
    for wi, m in enumerate(maps):
        if world_levels[wi]: want |= set(WORLD_MAPS.get(m['name'], ()))
    mrec = b''
    for mi in range(13):
        if mi in want:
            img, pal = map_screen_image(L, mi)
            bank, (o_img, o_pal) = into_bank(lambda bk: (bk.add(img), bk.add(b''.join(be16(c) for c in pal))))
            mrec += be32(o_img) + be32(o_pal) + bytes([7 + bank, 0, 0, 0])
        else:
            mrec += b'\0' * 12
    off_maps = p.add(mrec)
    # cabins (3 x u32 image, u32 palette, u8 bank, s8 BG3 h offset, 2 pad) + the dialogue font (96 x 16 B)
    srec = b''
    for m in SCREENS:
        img, pal = screen_image(L, m)
        bank, (o_img, o_pal) = into_bank(lambda bk: (bk.add(img), bk.add(b''.join(be16(c) for c in pal))))
        srec += be32(o_img) + be32(o_pal) + bytes([7 + bank, m['txh'] & 0xFF, 0, 0])
    off_screens = p.add(srec)
    p.align(4)
    off_tfont = p.add(L.assets['GFX_Sprite_AreaNameFont'][4][:1536])
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'audio'))
    import mkmusic
    if 'MUSIC_SONGS' not in os.environ:          # only the songs of the levels built (+ jingles / cabins)
        ents = set(b['lv']['entrance'] for b in built)
        os.environ['MUSIC_SONGS'] = ','.join(n for n, e in mkmusic.LEVEL_MUSIC.items() if ents & set(e))
    music, mnames = mkmusic.build_music(L.rom)
    print('music: %d KB, %s' % (len(music) // 1024, ' '.join(mnames)))
    # LEVEL_MUSIC is only the level-viewer menu's song table.  The complete game plays the song the
    # game itself uploads (bonus rooms included), so a room missing here is not a problem.
    nomus = [b for b in built if not any(b['lv']['entrance'] in v for v in mkmusic.LEVEL_MUSIC.values())]
    if nomus:
        print('note: %d rooms have no viewer-menu song (the game picks their music itself)' % len(nomus))
    p.align(16)
    off_music = p.add(music)
    # entrance -> pack level (room): the level itself, its aliases (bonus exits, checkpoints)
    room_of = bytearray([0xFF] * 256)
    for li, b in enumerate(built):
        room_of[b['lv']['entrance'] & 0xFF] = li
    for li, b in enumerate(built):
        for a in b['lv'].get('aliases', []):
            if room_of[a & 0xFF] == 0xFF: room_of[a & 0xFF] = li
    off_rooms = p.add(bytes(room_of))
    # the power-on snapshot (complete game: tools/capture.py boot_snapshot -> mkgame snap_100.bin), resident
    bp = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'game', 'snap_100.bin')
    off_boot = p.add(open(bp, 'rb').read(), 4) if os.path.exists(bp) and os.environ.get('WORLD_PROFILES') else 0
    # the front-end screens (tools/capture.py fe_capture: survey/fe): per screen (NMI routine $1C, BG
    # mode) its VRAM (64 KB, banked) - the 32X draws the BGs live from it (sh/fescr.c).  Resident table:
    # u32 n, n x (u16 nmi, u8 mode, u8 bank, u32 off_vram)
    off_fe = 0
    fj = os.path.join(paths.SURVEY, 'fe', 'fe.json')
    if off_boot and os.path.exists(fj):
        frec = b''
        fl = json.load(open(fj))
        fmd = b''
        for e in fl:
            vr = open(os.path.join(paths.SURVEY, 'fe', e['name'] + '.vram'), 'rb').read()
            assert len(vr) == 65536
            md = None
            if e['nmi'] in FE_GPLANE and e['mode'] < 0x80:
                cgp = os.path.join(paths.SURVEY, 'fe', e['name'] + '.cgram')
                rgp = os.path.join(paths.SURVEY, 'fe', e['name'] + '.regs')
                if os.path.exists(cgp) and os.path.exists(rgp):
                    md = fe_bg2_plane(vr, open(cgp, 'rb').read(), open(rgp, 'rb').read())
            # (its Genesis plane block in the same bank: window 7 shows both while the screen is up)
            bank, (o_v, o_md) = into_bank(lambda bk: (bk.add(vr, 16), bk.add(md, 4) if md else 0))
            # (an entrance-keyed screen - the credits: mode 0x80 | its world bank, patched in below)
            frec += be16(e['nmi']) + bytes([e['mode'] if e['mode'] < 0x80 else 0x80, 7 + bank]) + be32(o_v)
            fmd += be32(o_md)
            if md:
                print('pack: front-end screen %s: BG2 as a Genesis plane (%d bytes)' % (e['name'], len(md)))
        p.align(4)
        # u32 n, n x (u16 nmi, u8 mode, u8 bank, u32 off_vram), then (I11) n x u32 off_md (0: none)
        off_fe = p.add(be32(len(fl)) + frec + fmd)
        print('pack: %d front-end screens' % len(fl))
    p.buf[0:52] = b'DKCP' + be32(nlev) + be32(nworld) + be32(off_levels) + be32(off_worlds) + be32(off_font) + \
        be32(off_music) + be32(off_rooms) + be32(off_maps) + be32(off_screens) + be32(off_tfont) + be32(off_boot) + \
        be32(off_fe)
    # all-worlds ROM (mkgame.py WORLD_PROFILES -> build/game/worlds.json, bank order = world list):
    # cart window 6 bank per world - bank 0 sits at physical bank 6 (0x300000), the others after the
    # level banks (finalize.py).  -> world_t pad[0], mapscr_t pad[0], level flags bits 16-21
    wj = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'game', 'worlds.json')
    if os.environ.get('WORLD_PROFILES') and os.path.exists(wj):
        wk = json.load(open(wj))['worlds']
        b6 = lambda K: 0 if K not in wk else 6 if wk.index(K) == 0 else 7 + len(banks) + wk.index(K) - 1
        for wi, m in enumerate(maps):
            p.buf[off_worlds + wi * WORLD_REC + 57] = b6(wi + 1)
            for mi in WORLD_MAPS.get(m['name'], ()):
                if mi < 13:
                    p.buf[off_maps + mi * 12 + 9] = b6(wi + 1)
        if off_fe:
            for i, e in enumerate(fl):
                if e['mode'] >= 0x80:
                    wi = world_of.get(e.get('world'))
                    K = wi + 1 if wi is not None else 7 if 'Galleon' in e.get('world', '') else None
                    if K is not None:
                        assert b6(K) < 0x40
                        p.buf[off_fe + 4 + i * 8 + 2] = 0x80 | b6(K)
        for li, b in enumerate(built):
            wi = world_of.get(b['lv']['world'])
            K = wi + 1 if wi is not None else 7 if 'Galleon' in b['lv']['world'] else None
            if K is not None:
                p.buf[off_levels + li * LEVEL_REC + LEVEL_REC - 15] |= b6(K)
        print('pack: world banks (window 6) %s' % ', '.join('world %d -> %d' % (K, b6(K)) for K in wk))
    open(os.path.join(out, 'pack.bin'), 'wb').write(p.buf)
    with open(os.path.join(out, 'banks.bin'), 'wb') as f:
        for bk in banks:
            f.write(bytes(bk.buf) + b'\xff' * (BANK_SIZE - len(bk.buf)))
    print('pack: %d levels, %d worlds, resident %d KB, %d banks (%s KB)' % (
        nlev, nworld, len(p.buf) // 1024, len(banks), ' '.join(str(len(bk.buf) // 1024) for bk in banks)))
    return p


if __name__ == '__main__':
    main()
