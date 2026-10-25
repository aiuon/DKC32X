#!/usr/bin/env python3
"""Convert Jungle Hijinxs data from the user's DKC (USA v1.0) ROM into
32X / Genesis formats for the level viewer.

Outputs (in OUTDIR):
  sh_assets.bin  - big-endian blob for the SH-2s (palette, sky, baked 8bpp tiles, 8x8 tilemap)
  md_assets.h    - C arrays for the 68000 (Genesis tiles, nametables, CRAM, per-line tables)
  preview.png    - what the converter thinks the level looks like (debug aid)
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dkcrom import Rom, decode_4bpp_tile, decode_2bpp_tile  # noqa: E402

# ---------------------------------------------------------------- ROM locations (USA v1.0)
GFX_FGBG_JUNGLE = 0xD58FC0     # compressed BG1/BG2 4bpp tiles (VRAM $3000)
LAYOUT_JUNGLE = 0xD90000       # chunk layout, column-major, 16 chunks per column
MAP32_JUNGLE = 0xD9A3C0        # 32x32 chunk definitions (16 SNES tilemap words each)
MAP32_JUNGLE_END = 0xD9ED00
PAL_JUNGLE = 0xB9A1DC          # 128 BG colours -> CGRAM 0
TM_LAYER3_JUNGLE = 0xCDF337    # BG3 tilemap, 32x6 entries at BG3 row 8
GFX_LAYER3_JUNGLE = 0xCDF4B7   # BG3 2bpp tiles
SKY_HDMA = 0xB996E6            # HDMA -> CGRAM colour 0 (sky gradient)
SKY_HDMA_LEN = 0x220
BG3COL_HDMA = 0xB998EA         # HDMA -> CGRAM colour 1 (canopy fog gradient)
BG3COL_HDMA_LEN = 0xA0
COLORMATH_TABLE = 0xB9D7AE     # per entrance: CGADSUB, fixed colour
ENTRANCE_JUNGLE_HIJINXS = 0x16

LEVEL_COLS = 168               # Jungle Hijinxs occupies layout columns 0..167
BG2_COL0 = 168                 # background strip copied into BG2 by CODE_B99412
CHUNK_ROWS = 16

# Genesis DAC levels (0..14 half-steps); normal = even, shadow = 0..7
GEN_LEVELS = [0, 29, 52, 70, 87, 101, 116, 130, 144, 158, 172, 187, 206, 228, 255]


def be16(v):
    return struct.pack(">H", v & 0xFFFF)


def snes8(c5):
    return (c5 << 3) | (c5 >> 2)


def nearest_level(target, shadow):
    best, bl = None, 0
    for L in range(8):
        lv = GEN_LEVELS[L] if shadow else GEN_LEVELS[L * 2]
        d = abs(lv - target)
        if best is None or d < best:
            best, bl = d, L
    return bl


def parse_hdma(rom, addr, length):
    """Mode-3 CGRAM HDMA table -> list of 224 colours (per screen line)."""
    data = rom.read(addr, length)
    out = []
    i = 0
    while i < len(data) and len(out) < 224:
        n = data[i]
        if n == 0:
            break
        if n & 0x80:  # repeat mode: one entry per line
            for _ in range(n & 0x7F):
                out.append(data[i + 3] | data[i + 4] << 8)
                i += 4
            i += 1
        else:
            col = data[i + 3] | data[i + 4] << 8
            out += [col] * n
            i += 5
    while len(out) < 224:
        out.append(out[-1])
    return out[:224]


class Level:
    def __init__(self, rom):
        self.rom = rom
        g = rom.decompress(GFX_FGBG_JUNGLE)
        self.ntiles = len(g) // 32
        self.tiles = [decode_4bpp_tile(g, i * 32) for i in range(self.ntiles)]
        self.pal = [rom.u16(PAL_JUNGLE + i * 2) for i in range(128)]
        m = rom.read(MAP32_JUNGLE, MAP32_JUNGLE_END - MAP32_JUNGLE)
        self.m32 = [m[i] | m[i + 1] << 8 for i in range(0, len(m), 2)]
        lay = rom.read(LAYOUT_JUNGLE, 0xA3C0)
        self.lay = [lay[i] | lay[i + 1] << 8 for i in range(0, len(lay), 2)]

    def tile_entry(self, tx, ty):
        """SNES tilemap entry at 8x8 tile coordinate (tx, ty), chunk flips applied."""
        cx, cy = tx >> 2, ty >> 2
        c = self.lay[cx * CHUNK_ROWS + cy]
        ci = c & 0x3FFF
        sx, sy = tx & 3, ty & 3
        if c & 0x4000:
            sx = 3 - sx
        if c & 0x8000:
            sy = 3 - sy
        e = self.m32[ci * 16 + sy * 4 + sx]
        return e ^ (c & 0xC000)

    def tile_pixels(self, e):
        t = e & 0x3FF
        px = self.tiles[t] if t < self.ntiles else [0] * 64
        rows = [px[r * 8:r * 8 + 8] for r in range(8)]
        if e & 0x4000:
            rows = [r[::-1] for r in rows]
        if e & 0x8000:
            rows = rows[::-1]
        return rows


# ---------------------------------------------------------------- SH-2 side
def build_sh(lv, outdir):
    W, H = LEVEL_COLS * 4, CHUNK_ROWS * 4
    baked = {}       # (tile, pal, hflip) -> index
    bake_data = []
    tmap = []
    for ty in range(H):
        for tx in range(W):
            e = lv.tile_entry(tx, ty)
            t, p, hf, vf = e & 0x3FF, (e >> 10) & 7, (e >> 14) & 1, (e >> 15) & 1
            key = (t, p, hf)
            if key not in baked:
                px = lv.tiles[t] if t < lv.ntiles else [0] * 64
                rows = [px[r * 8:r * 8 + 8] for r in range(8)]
                if hf:
                    rows = [r[::-1] for r in rows]
                b = bytes((p * 16 + v) if v else 0 for r in rows for v in r)
                baked[key] = len(bake_data)
                bake_data.append(b)
            tmap.append(baked[key] | (vf << 15))
    # 32X palette: entry 0 = sky (rewritten per line), BG colours with the
    # priority bit set so BG1 always covers the Genesis layers.
    pal = [0] * 256
    for i in range(128):
        if i % 16:
            pal[i] = lv.pal[i] | 0x8000
    pal[254] = 0x0000 | 0x8000   # HUD shadow
    pal[255] = 0x7FFF | 0x8000   # HUD text
    sky = parse_hdma(lv.rom, SKY_HDMA, SKY_HDMA_LEN)

    hdr_size = 64
    off_pal = hdr_size
    off_sky = off_pal + 512
    off_tiles = off_sky + 224 * 2
    off_map = off_tiles + len(bake_data) * 64
    blob = bytearray()
    blob += b"DKC1"
    blob += struct.pack(">IIIIIIII", len(bake_data), off_tiles, W, H, off_map, off_pal, off_sky, 0)
    blob += struct.pack(">HHHH", LEVEL_COLS * 32, CHUNK_ROWS * 32, 0, 176)
    blob += b"\0" * (hdr_size - len(blob))
    for c in pal:
        blob += be16(c)
    for c in sky:
        blob += be16(c)
    for b in bake_data:
        blob += b
    for v in tmap:
        blob += be16(v)
    open(os.path.join(outdir, "sh_assets.bin"), "wb").write(blob)
    print("SH-2 assets: %d baked tiles (%d KB), map %dx%d, total %d KB"
          % (len(bake_data), len(bake_data) * 64 // 1024, W, H, len(blob) // 1024))
    return baked, bake_data, tmap, W, H, pal, sky


# ---------------------------------------------------------------- 68000 side
def gen_color(rgb5, darken_fixed=None, shadow=True):
    out = 0
    for i, s in enumerate((0, 5, 10)):
        c = (rgb5 >> s) & 31
        if darken_fixed is not None:
            c = max(c - darken_fixed[i], 0) / 2.0
        L = nearest_level(c * 255.0 / 31.0, shadow)
        out |= L << (1 + i * 4)
    return out


def build_md(lv, outdir):
    rom = lv.rom
    cm = rom.read(COLORMATH_TABLE + ENTRANCE_JUNGLE_HIJINXS * 4, 4)
    fixed = cm[1] | cm[2] << 8
    fixed3 = (fixed & 31, (fixed >> 5) & 31, (fixed >> 10) & 31)
    assert cm[0] == 0xC2, "unexpected colour math for Jungle Hijinxs"

    # ---- BG3 (canopy band) -> plane B, palette line 0 colours 1..3
    bg3cols = parse_hdma(rom, BG3COL_HDMA, BG3COL_HDMA_LEN)
    bg3_line_cram = [gen_color(c) for c in bg3cols]
    cram = [0] * 64
    cram[1] = bg3_line_cram[0]
    cram[2] = gen_color(lv.pal[2])
    cram[3] = gen_color(lv.pal[3])

    # ---- BG2 (jungle backdrop) tiles: unique (tile, snes palette)
    # CODE_B99412 streams level columns $14F8..$16F8 into the 64-column BG1
    # tilemap (then copies it to BG2); the tilemap wraps, so BG2 column c
    # holds level tile column 672 + ((c + 32) & 63).
    bg2_entries = [[lv.tile_entry(BG2_COL0 * 4 + ((x + 32) & 63), y) for x in range(64)] for y in range(32)]
    combos = sorted({(e & 0x3FF, (e >> 10) & 7) for row in bg2_entries for e in row})
    combo_cols = {}
    for t, p in combos:
        px = lv.tiles[t] if t < lv.ntiles else [0] * 64
        combo_cols[(t, p)] = {v: gen_color(lv.pal[p * 16 + v], fixed3) for v in set(px) if v}
    # pack colour sets into palette lines (line 0 has 12 free slots, 1..3 have 15)
    lines = [set(), set(), set(), set()]
    cap = [12, 15, 15, 15]
    assign = {}
    order = sorted(combos, key=lambda k: -len(set(combo_cols[k].values())))
    for k in order:
        need = set(combo_cols[k].values())
        best = None
        for li in (1, 2, 3, 0):
            grow = len(need - lines[li])
            if len(lines[li]) + grow <= cap[li] and (best is None or grow < best[0]):
                best = (grow, li)
        if best is None:  # no room: pick the line with most overlap, remap later
            best = (0, max(range(4), key=lambda li: len(need & lines[li])))
        else:
            lines[best[1]] |= need
        assign[k] = best[1]
    line_list = []
    for li in range(4):
        cols = sorted(lines[li])
        base = 4 if li == 0 else 1
        line_list.append({c: base + i for i, c in enumerate(cols)})
        for c, idx in line_list[li].items():
            cram[li * 16 + idx] = c

    def nearest_in_line(li, c):
        def rgb(v):
            return [(v >> (1 + i * 4)) & 7 for i in range(3)]
        cr = rgb(c)
        return min(line_list[li].items(), key=lambda kv: sum((a - b) ** 2 for a, b in zip(rgb(kv[0]), cr)))[1]

    gtiles = [bytes(32)]  # tile 0 = blank
    combo_index = {}
    for k in combos:
        t, p = k
        li = assign[k]
        px = lv.tiles[t] if t < lv.ntiles else [0] * 64
        idx = []
        for v in px:
            if not v:
                idx.append(0)
            else:
                c = combo_cols[k][v]
                idx.append(line_list[li].get(c) or nearest_in_line(li, c))
        b = bytes((idx[i] << 4) | idx[i + 1] for i in range(0, 64, 2))
        combo_index[k] = len(gtiles)
        gtiles.append(b)
    plane_a = []
    for row in bg2_entries:
        for e in row:
            k = (e & 0x3FF, (e >> 10) & 7)
            v = combo_index[k] | (assign[k] << 13)
            if e & 0x4000:
                v |= 0x0800
            if e & 0x8000:
                v |= 0x1000
            plane_a.append(v)

    # ---- BG3 tiles + plane B
    g3 = rom.read(GFX_LAYER3_JUNGLE, 0x450)
    tm3 = rom.read(TM_LAYER3_JUNGLE, 0x180)
    ents3 = [tm3[i] | tm3[i + 1] << 8 for i in range(0, len(tm3), 2)]
    bg3_base = len(gtiles)
    used3 = sorted({e & 0x3FF for e in ents3})
    bg3_index = {}
    for t in used3:
        px = decode_2bpp_tile(g3, t * 16)
        if not any(px):
            bg3_index[t] = 0
            continue
        bg3_index[t] = len(gtiles)
        gtiles.append(bytes((px[i] << 4) | px[i + 1] for i in range(0, 64, 2)))
    plane_b = [0] * (64 * 32)
    for i, e in enumerate(ents3):
        y = 8 + i // 32
        x = i % 32
        v = bg3_index[e & 0x3FF]
        if v:
            if e & 0x4000:
                v |= 0x0800
            if e & 0x8000:
                v |= 0x1000
        plane_b[y * 64 + x] = v
        plane_b[y * 64 + x + 32] = v

    def carr(name, typ, vals, per=12):
        s = "static const %s %s[%d] = {\n" % (typ, name, len(vals))
        for i in range(0, len(vals), per):
            s += "  " + ",".join("0x%04X" % v for v in vals[i:i + per]) + ",\n"
        return s + "};\n"

    tile_words = []
    for b in gtiles:
        tile_words += [b[i] << 8 | b[i + 1] for i in range(0, 32, 2)]
    h = "/* generated by build_assets.py from the user's ROM - do not distribute */\n"
    h += "#define MD_NUM_TILES %d\n#define MD_BG3_BASE %d\n" % (len(gtiles), bg3_base)
    h += carr("md_tiles", "uint16_t", tile_words)
    h += carr("md_plane_a", "uint16_t", plane_a, 16)
    h += carr("md_plane_b", "uint16_t", plane_b, 16)
    h += carr("md_cram", "uint16_t", cram, 16)
    h += carr("md_bg3_col", "uint16_t", bg3_line_cram, 16)
    open(os.path.join(outdir, "md_assets.h"), "w").write(h)
    print("68K assets: %d Genesis tiles, BG2 palette lines used %s, colour math fixed=%s"
          % (len(gtiles), [len(l) for l in lines], fixed3))
    return gtiles, plane_a, plane_b, cram


def preview(lv, outdir, sh, md):
    try:
        from PIL import Image
    except ImportError:
        return
    baked, bake_data, tmap, W, H, pal, sky = sh
    gtiles, plane_a, plane_b, cram = md

    def c32(w):
        return ((w & 31) * 255 // 31, ((w >> 5) & 31) * 255 // 31, ((w >> 10) & 31) * 255 // 31)

    def cgen(w, shadow=True):
        return tuple(GEN_LEVELS[((w >> (1 + i * 4)) & 7) * (1 if shadow else 2)] for i in range(3))
    camx, camy = 0, 176
    img = Image.new("RGB", (320, 224))
    pix = img.load()
    for y in range(224):
        for x in range(320):
            # 32X BG1
            lx, ly = camx + x, camy + y + 1
            e = tmap[(ly >> 3) * W + (lx >> 3)]
            b = bake_data[e & 0x7FFF]
            r = ly & 7
            if e & 0x8000:
                r = 7 - r
            v = b[r * 8 + (lx & 7)]
            if v:
                pix[x, y] = c32(pal[v])
                continue
            # plane A (BG2)
            ax, ay = (x + camx // 2) & 511, (y + camy // 4 + 1) & 255
            ent = plane_a[(ay >> 3) * 64 + (ax >> 3)]
            tb = gtiles[ent & 0x7FF]
            tx, ty = ax & 7, ay & 7
            if ent & 0x800:
                tx = 7 - tx
            if ent & 0x1000:
                ty = 7 - ty
            byte = tb[ty * 4 + tx // 2]
            ci = (byte >> 4) if tx % 2 == 0 else byte & 15
            if ci:
                pix[x, y] = cgen(cram[((ent >> 13) & 3) * 16 + ci])
                continue
            # plane B (BG3)
            band = min(y // 8 - 9, 3) if y >= 72 else 0
            band = max(band, 0)
            bx = (x + (camx * (2 + band)) // 16) & 255
            ent = plane_b[(y >> 3) * 64 + (bx >> 3)]
            tb = gtiles[ent & 0x7FF]
            tx, ty = bx & 7, y & 7
            if ent & 0x800:
                tx = 7 - tx
            if ent & 0x1000:
                ty = 7 - ty
            byte = tb[ty * 4 + tx // 2]
            ci = (byte >> 4) if tx % 2 == 0 else byte & 15
            if ci:
                pix[x, y] = cgen(cram[ci])
                continue
            pix[x, y] = c32(sky[y])
    img.save(os.path.join(outdir, "preview.png"))


def main():
    if len(sys.argv) < 3:
        print("usage: build_assets.py <Donkey Kong Country (USA).sfc> <outdir>")
        sys.exit(1)
    rom = Rom(sys.argv[1])
    rom.check()
    os.makedirs(sys.argv[2], exist_ok=True)
    lv = Level(rom)
    sh = build_sh(lv, sys.argv[2])
    md = build_md(lv, sys.argv[2])
    preview(lv, sys.argv[2], sh, md)


if __name__ == "__main__":
    main()
