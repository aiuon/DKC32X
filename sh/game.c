/*
 * DKC game logic on the SH-2: the SNES 65816 code, statically recompiled (tools/recomp/r65.py,
 * hot functions) or interpreted (r65interp.c, everything else), driven one frame at a time:
 * NMI entry (00:A968) -> the game's frame routine -> its WAI.  This file: the memory map seen by
 * that code and the SNES I/O registers it touches (joypad, APU ports, multiply / divide, DMA, PPU).
 */
#define R65_REGS
#include "../tools/recomp/r65rt.h"
#include "game.h"
#include "sprites.h"

/* ------------------------------------------------------------ memory */
uint8_t wram_lo[0x2000] __attribute__((aligned(16)));
uint8_t wram_dummy[256] __attribute__((aligned(16)));
/* wram_pool[], wram_hp[] and snes_pg[] come from the generated build/game/game_tab.c */

static cpu_t cpu;
void run65_safe(cpu_t *c, uint32_t key);
static uint8_t regs[0x200];            /* $2100-$21FF -> [0x000-0x0FF], $4200-$42FF / $4300-$437F -> [0x100-0x1FF] */
#define PPUREG(a) regs[(a) - 0x2100]
#define CPUREG(a) regs[0x100 + ((a) & 0xFF)]
#define DMAREG(ch, r) regs[0x180 + (ch) * 16 + (r)]   /* 4300-437F folded into 0x180-0x1FF */
static uint32_t joy1;
static uint32_t wmadd;
static uint32_t apu_port[4];
int game_sfx_on = 1;
#ifdef __sh__
#include "music.h"
#else
#include <stdio.h>
#include <stdlib.h>
static void music_sfx(uint32_t c, uint32_t p) { if (getenv("SFXLOG")) printf("  sfx %02X %02X f %u\n", c, p, game_stats.frames); }
#endif
static uint32_t m7_a;
static uint8_t sram[0x800];
int game_sram_dirty;                     /* the game wrote new save data (main.c writes it to the cart) */
uint8_t *game_sram(void) { return sram; }
game_stats_t game_stats;

/* APU: the sound driver echoes port 0; port 1 reads $FF (song upload done).  The command bytes
   are queued for the sound-effect player (not yet). */
R65_HOT static void apu_write(uint32_t port, uint32_t v)
{
    /* port 0 changes = a new command (port 1 = cmd, port 2 = param): sound effects go to the
       32X engine (songs are chosen by the 32X side from the level) */
    if (port == 0 && v != apu_port[0]) {
        game_stats.apu_cmds++;
        if (game_sfx_on) music_sfx(apu_port[1], apu_port[2]);
    }
    apu_port[port] = v;
}

uint32_t apu_port0_echo(uint32_t v) { apu_port[0] = v; return v; }

static uint32_t rd_dma_src(uint32_t a) { return mrd8(a); }

R65_HOT static void do_dma(uint32_t mask)
{
    for (int ch = 0; ch < 8; ch++) {
        if (!(mask >> ch & 1)) continue;
        uint32_t dmap = DMAREG(ch, 0), bb = DMAREG(ch, 1);
        uint32_t src = DMAREG(ch, 2) | DMAREG(ch, 3) << 8, bank = DMAREG(ch, 4);
        uint32_t n = DMAREG(ch, 5) | DMAREG(ch, 6) << 8;
        if (!n) n = 0x10000;
        game_stats.dmas++;
        if (bb == 0x80 && !(dmap & 0x80)) {
            /* A -> WRAM port */
            int step = (dmap & 0x08) ? 0 : (dmap & 0x10) ? -1 : 1;
            for (uint32_t k = 0; k < n; k++) {
                uint32_t v = rd_dma_src(bank << 16 | src);
                if (wmadd < 0x2000) wram_lo[wmadd] = v; else WH(wmadd) = v;
                wmadd = (wmadd + 1) & 0x1FFFF;
                src = (src + step) & 0xFFFF;
            }
        } else {
            game_dma_hook(dmap, bb, bank << 16 | src, n);
            if (!(dmap & 0x08)) src = (dmap & 0x10) ? (src - n) & 0xFFFF : (src + n) & 0xFFFF;
        }
        DMAREG(ch, 2) = src; DMAREG(ch, 3) = src >> 8; DMAREG(ch, 5) = DMAREG(ch, 6) = 0;
    }
}

R65_HOT uint32_t io_rd(uint32_t ea)
{
    uint32_t a = ea & 0xFFFF;
    if (a >= 0x6000) {                      /* SRAM (banks 20-3F) */
        uint32_t bank = (ea >> 16) & 0x7F;
        if (bank >= 0x20 && bank < 0x40) return sram[a & 0x7FF];
        return 0;
    }
    switch (a) {
    case 0x2140: return apu_port[0];
    case 0x2141: return 0xFF;
    case 0x2142: return apu_port[2];
    case 0x2143: return apu_port[3];
    case 0x4210: return 0x02;
    case 0x4211: return 0;
    case 0x4212: return 0x80;
    case 0x4214: case 0x4215: case 0x4216: case 0x4217: return CPUREG(a);
    case 0x4218: return joy1 & 0xFF;
    case 0x4219: return joy1 >> 8;
    case 0x421A: case 0x421B: return 0;
    case 0x4016: case 0x4017: return 1;
    case 0x2137: case 0x213F: return 0;
    case 0x213C: case 0x213D: return 0xE1;
    case 0x2134: case 0x2135: case 0x2136: {
        int32_t r = (int16_t)m7_a * (int8_t)PPUREG(0x211C);
        return (r >> ((a - 0x2134) * 8)) & 0xFF; }
    case 0x2180: { uint32_t v = wmadd < 0x2000 ? wram_lo[wmadd] : WH(wmadd); wmadd = (wmadd + 1) & 0x1FFFF; return v; }
    }
    if (a >= 0x4300 && a < 0x4380) return DMAREG((a >> 4) & 7, a & 15);
    if (a >= 0x2100 && a < 0x2200) return 0;
    return 0;
}

/* INIDISP: last value, and "a frame ended with the screen forced blank" (sticky, cleared by main.c once
   it has reloaded the tile cache: the load-only WRAM pages alias it, see mkgame.py; the NMI blanks
   the screen during its DMAs every frame, so only the value left at the end of a frame counts) */
uint8_t game_inidisp = 0x0F, game_fblank;

#ifdef IOCOUNT
uint32_t io_wcnt[0x10000], io_rcnt[0x10000];
#endif
R65_HOT void io_wr(uint32_t ea, uint32_t v)
{
    uint32_t a = ea & 0xFFFF;
#ifdef IOCOUNT
    io_wcnt[a]++;
#endif
    v &= 0xFF;
    if (a >= 0x6000) {
        uint32_t bank = (ea >> 16) & 0x7F;
        if (bank >= 0x20 && bank < 0x40 && sram[a & 0x7FF] != v) { sram[a & 0x7FF] = v; game_sram_dirty = 1; }
        return;
    }
    if (a >= 0x2100 && a < 0x2200) {
        PPUREG(a) = v;
        switch (a) {
        case 0x2140: case 0x2141: case 0x2142: case 0x2143: apu_write(a & 3, v); break;
        case 0x2180: if (wmadd < 0x2000) wram_lo[wmadd] = v; else WH(wmadd) = v; wmadd = (wmadd + 1) & 0x1FFFF; break;
        case 0x2181: wmadd = (wmadd & 0x1FF00) | v; break;
        case 0x2182: wmadd = (wmadd & 0x100FF) | v << 8; break;
        case 0x2183: wmadd = (wmadd & 0x0FFFF) | (v & 1) << 16; break;
        case 0x211B: m7_a = (m7_a >> 8) | v << 8; fe_reg(a, v); break;
        case 0x2100: game_inidisp = v; break;
        default: game_ppu_hook(a, v); break;
        }
        return;
    }
    if (a >= 0x4300 && a < 0x4380) { DMAREG((a >> 4) & 7, a & 15) = v; return; }
    if (a >= 0x4200 && a < 0x4300) {
        CPUREG(a) = v;
        switch (a) {
        case 0x4203: { uint32_t r = CPUREG(0x4202) * v; CPUREG(0x4216) = r; CPUREG(0x4217) = r >> 8; break; }
        case 0x4206: {
            uint32_t dvd = CPUREG(0x4204) | CPUREG(0x4205) << 8, q, r;
            if (v) { q = dvd / v; r = dvd % v; } else { q = 0xFFFF; r = dvd; }
            CPUREG(0x4214) = q; CPUREG(0x4215) = q >> 8; CPUREG(0x4216) = r; CPUREG(0x4217) = r >> 8; break; }
        case 0x420B: do_dma(v); break;
        }
    }
}

/* ------------------------------------------------------------ control */
/* snapshot: 8 x u32 (A X Y S D DB P pc) big-endian, WRAM 0-1FFF, then the resident high pages in
   wram_pool order, the ring-alias pages (class 16), then 2 KB SRAM */
/* snapshot WRAM (lo 8 KB, pool pages, 2 KB SRAM) as one byte stream (mkgame.py lz_pack) */
static uint8_t *snap_byte(uint32_t i)
{
    if (i < 0x2000) return &wram_lo[i];
    i -= 0x2000;
    if (i < (uint32_t)game_npool * 256) return &wram_pool[i >> 8][i & 255];
    i -= game_npool * 256;
    if (i < (uint32_t)game_nring * 256) { extern uint32_t ring_ent[]; return (uint8_t *)ring_ent + i; }
    return &sram[i - game_nring * 256];
}
/* (stops at n: bytes past it are not written - a level start keeps the save RAM, the stream's end) */
static void lz_unpack(const uint8_t *z, uint32_t n)
{
    uint32_t o = 0;
    while (o < n) {
        uint32_t t = *z++;
        if (t < 0x80) { for (uint32_t k = 0; k <= t; k++, o++, z++) if (o < n) *snap_byte(o) = *z; }
        else {
            uint32_t l = (t & 0x7F) + 3, d = z[0] << 8 | z[1]; z += 2;
            for (uint32_t k = 0; k < l && o < n; k++, o++) *snap_byte(o) = *snap_byte(o - d);
        }
    }
}
int game_keep_sram;                      /* (I6) game_init: keep the save RAM (level starts) */

void game_init(const uint8_t *snap, uint32_t entrance)
{
    const uint32_t *h0 = (const uint32_t *)snap;
    uint32_t h[8];
    for (int i = 0; i < 8; i++) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        h[i] = __builtin_bswap32(h0[i]);          /* (host simulation) */
#else
        h[i] = h0[i];
#endif
    }
    cpu.a = h[0]; cpu.x = h[1]; cpu.y = h[2]; cpu.s = h[3]; cpu.d = h[4]; cpu.db = h[5]; cpu.p = h[6]; cpu.pc = h[7];
    const uint8_t *p = snap + 32;
    uint32_t zlen = p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
    /* (I6) the snapshot's save RAM is the capture's (empty files): a level start from the menu wrote
       it over the player's saves - kept unless this is the power-on snapshot */
    lz_unpack(p + 4, 0x2000 + (game_npool + game_nring) * 256 + (game_keep_sram ? 0 : 0x800));
    p += (4 + zlen + 3) & ~3;
    {   /* OBJ VRAM: tile indices into the resident pool (mkgame.py), CGRAM after the WRAM */
        extern const uint16_t *const game_snap_obj[], game_snap_ent[];
        int k = 0;
        while (game_snap_ent[k] != 0xFFFF && game_snap_ent[k] != entrance) k++;
        if (game_snap_ent[k] == 0xFFFF) k = 0;
        spr_init(game_snap_obj[k], p);
    }
    for (int i = 0; i < 0x200; i++) regs[i] = 0;
    apu_port[0] = wram_lo[0];
    wmadd = 0;
    game_stats.err = 0;
}

static uint32_t dbg_jump;
/* SNES pad bits as read from $4218 (16-bit): B Y Sl St U D L R A X L R 0 0 0 0 */
R65_HOT uint32_t game_frame(uint32_t snes_pad)
{
    joy1 = snes_pad;
    if (game_stats.err) return game_stats.err;
    /* NMI: push PB, PC, P; I = 1, D = 0 */
    uint32_t s = cpu.s, pc = cpu.pc;
    wram_lo[s] = (pc >> 16) & 0xFF; s = (s - 1) & 0xFFFF;
    wram_lo[s] = (pc >> 8) & 0xFF; s = (s - 1) & 0xFFFF;
    wram_lo[s] = pc & 0xFF; s = (s - 1) & 0xFFFF;
    wram_lo[s] = cpu.p; s = (s - 1) & 0xFFFF;
    cpu.s = s; cpu.p = (cpu.p | 0x04) & ~0x08;
    { extern uint32_t r65_ibudget; r65_ibudget = 0; }
    if (dbg_jump) { uint32_t k = dbg_jump; dbg_jump = 0; run65_safe(&cpu, k); } else   /* (game_debug_jump) */
    run65_safe(&cpu, ((cpu.p & 0x30) << 20) | 0x00A968);
    if (cpu.err) { game_stats.err = cpu.errpc; game_stats.errpc = cpu.errpc; }
    /* the scratch pages (tile cache) are only used by loads and the transition screens: anything
       but the main loop's WAI (8080F8) or a dark screen holds the renderer and reloads afterwards */
    if (game_dark() || (cpu.pc & 0xFFFFFF) != 0x8080F8) game_fblank = 1;
    game_stats.frames++;
    return game_stats.err;
}

int game_in_main_loop(void) { return (cpu.pc & 0xFFFFFF) == 0x8080F8; }
uint32_t game_wai_pc(void) { return cpu.pc & 0xFFFFFF; }
/* (test hook) the game continues at this key after the next NMI: e.g. 0x0080806F = load entrance $3E */
void game_debug_jump(uint32_t key) { dbg_jump = key; }

/* the backdrop colour per line from the game's CGRAM HDMA (an enabled channel to $2121 in mode 3:
   CGADD, CGADD, colour lo, hi); 0 if there is none.  Lines without a write keep the last colour. */
int game_hdma_sky(uint32_t table, uint16_t *out, int nlines)
{
    /* (the level-start snapshots hold no DMA registers: the caller names the table) */
    if (table < 0x1C00) {                 /* in WRAM 0-1FFF (the jungle sky at $1B41): direct reads */
        const uint8_t *p = &wram_lo[table], *e = &wram_lo[0x1FF8];
        uint32_t col = 0, line = 0;
        while (line < (uint32_t)nlines && p < e) {
            uint32_t c = *p++;
            if (!c) break;
            uint32_t n = c & 0x7F, rep = c & 0x80;
            if (!n) n = 128;
            if (!rep) {
                if (p[0] == 0) col = (p[2] | p[3] << 8) & 0x7FFF;
                p += 4;
                for (uint32_t k = 0; k < n && line < (uint32_t)nlines; k++) out[line++] = (uint16_t)col;
            } else {
                for (uint32_t k = 0; k < n && line < (uint32_t)nlines && p < e; k++, p += 4) {
                    if (p[0] == 0) col = (p[2] | p[3] << 8) & 0x7FFF;
                    out[line++] = (uint16_t)col;
                }
            }
        }
        while (line < (uint32_t)nlines) out[line++] = (uint16_t)col;
        return 1;
    }
    {
        uint32_t a = table & 0xFFFF, bank = table >> 16;
        uint32_t col = 0, line = 0;
        while (line < (uint32_t)nlines) {
            uint32_t c = mrd8(bank << 16 | a); a = (a + 1) & 0xFFFF;
            if (!c) break;
            uint32_t n = c & 0x7F, rep = c & 0x80;
            if (!n) n = 128;
            for (uint32_t k = 0; k < n && line < (uint32_t)nlines; k++) {
                if (k == 0 || rep) {
                    uint32_t ad = mrd8(bank << 16 | a);
                    uint32_t lo = mrd8(bank << 16 | ((a + 2) & 0xFFFF)), hi = mrd8(bank << 16 | ((a + 3) & 0xFFFF));
                    if (ad == 0) col = (lo | hi << 8) & 0x7FFF;
                    if (rep) a = (a + 4) & 0xFFFF;
                }
                out[line++] = (uint16_t)col;
            }
            if (!rep) a = (a + 4) & 0xFFFF;
        }
        while (line < (uint32_t)nlines) out[line++] = (uint16_t)col;
        return 1;
    }
    return 0;
}
uint32_t game_wram16(uint32_t a) { return wram_lo[a] | wram_lo[a + 1] << 8; }
/* Torchlight Trouble: Squawks' flashlight is window 1 (BG3 beam + colour math), HDMA channel 2 from the
   table at $7F1C28 (CODE_B9887B: 224 x [1, WH0, WH1]; CODE_819370 rebuilds it every frame from
   Squawks' pose: apex = his position, slopes per pose, mirrored when he faces left: then WH0 = 0).
   -> LR[2y], LR[2y + 1] = the first and last lit 32X x of picture line y, the SNES x + xoff (the 32X
   view's left edge in SNES x: spr_xoff); open sides = -0x7FFF / 0x7FFF, a line without light is
   [0x7FFF, 0x7FFF] in a right-facing frame, [-0x7FFF, -0x7FFF] in a left-facing one.  The SNES window
   ends at x 255: the 32X's extra columns continue the beam (right-facing: above the first lit line the
   top edge goes on with its slope).  Returns the kind: 1 every lit line open on the left, 2 open on
   the right (also: no light at all), 3 other; *pax / *pay = the apex (32X x of the edge nearest the
   bird, its topmost line). */
int game_beam_lines(int16_t *LR, int32_t xoff, int nlines, int32_t *pax, int32_t *pay)
{
    int nl = 0, nr = 0, n = 0, f = -1, ay = -1;
    int32_t ax = 0x7FFF, axl = -1, ayl = -1;
#ifdef BMFLIP
    int32_t fx = 255;                     /* (test: the table mirrored about its apex = a left-facing bird) */
    for (int y = 0; y < nlines; y++) {
        uint32_t a = 0x11C28 + (uint32_t)y * 3;
        if (WH(a) == 1 && WH(a + 1) <= WH(a + 2) && (int32_t)WH(a + 1) < fx) fx = WH(a + 1);
    }
    fx += 60;
#endif
    for (int y = 0; y < nlines; y++) {
        uint32_t a = 0x11C28 + (uint32_t)y * 3;
        uint32_t c = WH(a), l0 = WH(a + 1), r0 = WH(a + 2);
#ifdef BMFLIP
        if (c == 1 && l0 <= r0) { int32_t m = 2 * fx - (int32_t)l0; r0 = m < 0 ? 1 : m > 254 ? 254 : (uint32_t)m; l0 = 0; if (m < 0) l0 = 255; }
#endif
        if (c != 1 || l0 > r0) { LR[2 * y] = LR[2 * y + 1] = 0x7FFF; continue; }
        n++;
        LR[2 * y] = (int16_t)(l0 ? (int32_t)l0 + xoff : -0x7FFF);
        LR[2 * y + 1] = (int16_t)(r0 < 255 ? (int32_t)r0 + xoff : 0x7FFF);
        if (r0 == 255) nr++;
        if (l0 == 0) nl++;
        if (f < 0) f = y;
        if ((int32_t)l0 < ax) { ax = (int32_t)l0; ay = y; }
        if ((int32_t)r0 > axl) { axl = (int32_t)r0; ayl = y; }
    }
    int kind = n == 0 || nr == n ? 2 : nl == n ? 1 : 3;
    if (kind == 1) {
        for (int y = 0; y < nlines; y++)
            if (LR[2 * y] == 0x7FFF) LR[2 * y] = LR[2 * y + 1] = -0x7FFF;
        *pax = axl + xoff; *pay = ayl;
        return kind;
    }
    if (kind == 2 && f > 0 && ay > f) {
        /* top edge: x grows by (L[f] - ax) / (ay - f) px per line upwards (16.8 fixed point) */
        int32_t s = (((int32_t)LR[2 * f] - (ax + xoff)) << 8) / (ay - f), x = (int32_t)LR[2 * f] << 8;
        for (int y = f - 1; y >= 0 && s > 0; y--) {
            x += s;
            if ((x >> 8) >= 320) break;
            LR[2 * y] = (int16_t)(x >> 8);
            LR[2 * y + 1] = 0x7FFF;
        }
    }
    *pax = ax + xoff; *pay = ay;
    return kind;
}
/* the last value written to a PPU ($21xx), CPU ($42xx) or DMA ($43xx) register */
uint32_t game_ppu(uint32_t a)
{
    if (a >= 0x2100 && a < 0x2200) return PPUREG(a);
    if (a >= 0x4300 && a < 0x4380) return DMAREG((a >> 4) & 7, a & 15);
    if (a >= 0x4200 && a < 0x4300) return CPUREG(a);
    return 0;
}
/* run the enabled HDMA channels' tables for one frame (as the PPU would, without timing): every
   register write -> wr(ctx, line, $21xx, value), in table order per channel */
void game_hdma_run(void (*wr)(void *, int, uint32_t, uint32_t), void *ctx, int nlines)
{
    static const uint8_t ulen[8] = { 1, 2, 2, 4, 4, 4, 2, 4 };
    static const uint8_t uoff[8][4] = { { 0 }, { 0, 1 }, { 0, 0 }, { 0, 0, 1, 1 }, { 0, 1, 2, 3 }, { 0, 1, 0, 1 },
                                        { 0, 0 }, { 0, 0, 1, 1 } };
    uint32_t en = CPUREG(0x420C);
    for (int ch = 0; ch < 8; ch++) {
        uint32_t dmap = DMAREG(ch, 0);
        if (!(en >> ch & 1) || (dmap & 0x80)) continue;
        uint32_t mode = dmap & 7, ind = dmap & 0x40, bb = DMAREG(ch, 1);
        uint32_t a = DMAREG(ch, 2) | DMAREG(ch, 3) << 8, bank = DMAREG(ch, 4) << 16, ib = DMAREG(ch, 7) << 16;
        int line = 0;
        while (line < nlines) {
            uint32_t c = mrd8(bank | a); a = (a + 1) & 0xFFFF;
            if (!c) break;
            uint32_t n = c & 0x7F, rep = c & 0x80, da, db;
            if (!n) n = 128;
            if (ind) { da = mrd8(bank | a) | mrd8(bank | ((a + 1) & 0xFFFF)) << 8; a = (a + 2) & 0xFFFF; db = ib; }
            else { da = a; db = bank; }
            for (uint32_t k = 0; k < n && line < nlines; k++, line++)
                if (k == 0 || rep)
                    for (uint32_t u = 0; u < ulen[mode]; u++) {
                        wr(ctx, line, 0x2100 + ((bb + uoff[mode][u]) & 0xFF), mrd8(db | da));
                        da = (da + 1) & 0xFFFF;
                    }
            if (!ind) a = da;
        }
    }
}

