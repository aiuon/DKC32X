/*
 * SNES OBJ layer on the 32X framebuffer: the game's OAM buffer (WRAM $0200), sprite graphics
 * tracked by VRAM DMA (pointers to the 4bpp planar source tiles), OBJ palettes -> CRAM 128-255.
 * Sprites are drawn into the back buffer through the overwrite region (zero bytes = transparent);
 * the canvas tiles they cover are marked and redrawn the next time that buffer is drawn.
 */
#define R65_REGS
#include "../tools/recomp/r65rt.h"
#include "game.h"
#include "sprites.h"
#ifdef VRAM_SHADOW
#include <string.h>
#endif

#define REG16(a)  (*(volatile uint16_t *)(a))
#ifndef OVR16
#define OVR16     ((volatile uint16_t *)0x24020000)
#endif
#ifndef MARKS_UNCACHED
#define MARKS_UNCACHED(p) ((uint32_t (*)[2])((uint32_t)(p) | 0x20000000))
#endif
#define SCREEN_W  320
#define SCREEN_H  224

/* VRAM OBJ tiles 0-511 (OBSEL name base 0, no gap): source pointers (32-byte 4bpp planar) */
static const uint8_t *obj_tile[512];
/* (I18) the renderers' copy of obj_tile (spr_prepare_b, when there is no chunky copy): in the 60 FPS
   mode the slave draws picture N while the master's logic N+1 re-points obj_tile (VRAM DMAs) - the
   sprites read the live table, so parts of them showed the next frame's graphics (flashing tile parts
   in the animal bonus rooms: the snow overlay leaves no chunky copy) */
static const uint8_t *obj_tile_dr[512];
static void obj_tile_snap(void)
{
#ifdef __sh__
    const uint8_t **d = (const uint8_t **)((uint32_t)obj_tile_dr | 0x20000000);   /* (write-through anyway) */
#else
    const uint8_t **d = obj_tile_dr;                                               /* (host simulation) */
#endif
    for (int t = 0; t < 512; t++) d[t] = obj_tile[t];
}
/* chunky copy of the OBJ tiles (16 KB, or 0): row r of tile t = obj_ck[t * 8 + r], 8 pixel nibbles,
   pixel 0 in the top nibble - converted when a tile is set, so the drawing loops read one long per
   row instead of four bitplane bytes and four spread lookups.  Borrowed from main.c (map_mem) for
   levels that leave it free. */
uint32_t *spr_ck;
#ifdef __sh__
#define SPR_UNC(p) ((__typeof__(p))((uint32_t)(p) | 0x20000000))
#else
#define SPR_UNC(p) (p)
#endif
static uint32_t spread[256];            /* bit i of a plane byte -> nibble i (pixel 7 - i): pixel 0 on top */
static inline uint32_t ck_row(const uint8_t *p, int r)
{
    return spread[p[2 * r]] | spread[p[2 * r + 1]] << 1 | spread[p[2 * r + 16]] << 2 | spread[p[2 * r + 17]] << 3;
}
R65_HOT static void ck_conv(uint32_t t, const uint8_t *p)
{
    uint32_t *d = SPR_UNC(spr_ck + t * 8);   /* uncached both ways (test 8 used cached copies + purges:
                                                sprite tiles scrambled on MiSTer while moving) */
    for (int r = 0; r < 8; r++) d[r] = ck_row(p, r);
}
/* tiles changed since the last spr_prepare (master only): converted there, then published for the
   slave, which purges their cache lines (cached reads instead of uncached ones: ~29 cycles each on
   MiSTer) */
#define CK_LMAX 256
static uint8_t ck_dirty[512];
static uint16_t ck_list[CK_LMAX];       /* (also read by the slave during the sprite job: the master
                                           only appends during the next frame's logic) */
static uint32_t ck_nlist;               /* > CK_LMAX: too many, the slave purges its whole cache */
#define CK_MARK(t) do { if (!ck_dirty[t]) { ck_dirty[t] = 1; if (ck_nlist < CK_LMAX) ck_list[ck_nlist] = (uint16_t)(t); ck_nlist++; } } while (0)
static uint8_t fill_tile[32];          /* fixed-source VRAM DMAs (clears): the tile of that byte */
R65_HOT static void obj_set(uint32_t t, const uint8_t *p)
{
    /* the same source again (the game re-sends a pose's graphics every frame it is shown): the
       chunky copy is still right - ROM and snapshot tiles never change; fill_tile does */
    if (obj_tile[t] == p && p != fill_tile) return;
    obj_tile[t] = p;
    CK_MARK(t);
}
/* (re)build the whole chunky copy (main.c lends / takes the buffer) */
void spr_ck_attach(uint32_t *buf)
{
    spr_ck = buf;
    ck_nlist = 0;
    for (int t = 0; t < 512; t++) ck_dirty[t] = 0;
    if (buf) for (int t = 0; t < 512; t++) ck_conv(t, obj_tile[t]);
    else obj_tile_snap();
}
static uint32_t vmadd, vmain;
static uint32_t cgadd, cg_latch, cg_phase;
uint16_t spr_pal[128];
uint16_t spr_bgpal[128];               /* the game's BG CGRAM colours 0-127 (bit 15 set) */
volatile uint32_t spr_bgpal_dirty;
volatile uint32_t spr_pal_dirty;       /* bit p = OBJ palette p changed */
uint32_t spr_marks[2][32][2];          /* per buffer: canvas tiles covered by sprites (row & 31, col & 63) */
spr_stats_t spr_stats;

void spr_tables(void);

static const uint8_t *snes_ptr(uint32_t a)
{
    uint32_t bank = a >> 16, off = a & 0xFFFF;
    if ((bank & 0xFE) == 0x7E) return &WH(a & 0x1FFFF);
    if (!(bank & 0x40) && off < 0x2000) return &wram_lo[off];
    return &ROMB(a & 0x3FFFFF);
}

static const uint16_t *snap_obj;      /* the level-start snapshot's OBJ VRAM as runs (mkgame.py, I16): */
extern const uint8_t game_obj_pool[];  /* blank tiles, tiles in resident ROM, tiles of a pool */
extern const uint8_t zero_page[];
/* the source of run tile i: 3 x u16 per run = kind << 14 | (count - 1), operand (32 bits) */
static const uint8_t *snap_run_tile(const uint16_t *r, uint32_t i)
{
    uint32_t kd = r[0] >> 14, op = (uint32_t)r[1] << 16 | r[2];
    if (kd == 0) return zero_page;
    if (kd == 1) return &ROMB((op + i * 32) & 0x3FFFFF);
    return game_obj_pool + (op + i) * 32;
}
static const uint8_t *snap_tile(uint32_t t)
{
    const uint16_t *r = snap_obj;
    t &= 511;
    for (;;) {
        uint32_t n = (r[0] & 0x3FFF) + 1;
        if (t < n) return snap_run_tile(r, t);
        t -= n; r += 3;
    }
}
#define SNAP_TILE(t) snap_tile(t)
uint8_t *spr_rwbuf;                    /* 16 KB for CPU-written OBJ tiles (set by main.c), or 0 */

/* the level renderer takes its buffer back: tiles written there -> the snapshot's */
void spr_rw_release(void)
{
    uint32_t lo = ((uint32_t)spr_rwbuf) | 0x20000000, hi = lo + 0x4000;
    for (int t = 0; t < 512; t++)
        if ((uint32_t)obj_tile[t] >= lo && (uint32_t)obj_tile[t] < hi) obj_set(t, SNAP_TILE(t));
}
void spr_init(const uint16_t *obj_idx, const uint8_t *cgram)
{
    snap_obj = obj_idx;
#ifdef VRAM_SHADOW
    { extern uint8_t shv[]; for (int t = 0; t < 512; t++) memcpy(shv + t * 32, SNAP_TILE(t), 32); }
#endif
    spr_tables();
    {   /* (I16) the runs in order (snap_tile walks them for one tile) */
        const uint16_t *r = snap_obj;
        for (uint32_t t = 0; t < 512; r += 3) {
            uint32_t n = (r[0] & 0x3FFF) + 1;
            for (uint32_t i = 0; i < n && t < 512; i++, t++) obj_tile[t] = snap_run_tile(r, i);
        }
    }
    if (spr_ck) spr_ck_attach(spr_ck);
    obj_tile_snap();
    for (int i = 0; i < 128; i++) {
        uint32_t c = cgram[(128 + i) * 2] | cgram[(128 + i) * 2 + 1] << 8;
        spr_pal[i] = (c & 0x7FFF) | 0x8000;
        c = cgram[i * 2] | cgram[i * 2 + 1] << 8;
        spr_bgpal[i] = (c & 0x7FFF) | 0x8000;
    }
    spr_bgpal_dirty = 0;
    spr_pal_dirty = 0xFF;
    for (int b = 0; b < 2; b++)
        for (int i = 0; i < 32; i++) spr_marks[b][i][0] = spr_marks[b][i][1] = 0;
    vmadd = 0; vmain = 0x80; cgadd = 0; cg_phase = 0;
}

#ifdef VRAMLOG
int vram_log;
#endif
/* ---- front-end screens (main.c fescr.c draws them): BG scroll / mode 7 / fixed colour registers and
   a VRAM mirror of the maps (words 7000-7FFF, in memory main.c lends while no level is shown) */
int spr_front_all;                       /* (fescr.c: sprites behind BG1 drawn in front - the credits' names, added to BG1 from the subscreen) */
uint16_t *fe_vm;
void (*fe_lend_hook)(void);              /* (main.c: lend the mirror now - a dark screen's first map write) */
static int16_t sext13(uint32_t v) { return (int16_t)((v & 0x1FFF) << 3) >> 3; }
void fe_reg(uint32_t reg, uint32_t v)
{
    if (!fe_vm) return;
    fe_ppu_t *p = &FE_TRK->ppu;
    if (reg <= 0x2114) {
        uint32_t i = (reg - 0x210D) >> 1, w = (v << 8 | p->bglatch) & 0x3FF;
        if ((reg - 0x210D) & 1) p->vofs[i] = (uint16_t)w; else p->hofs[i] = (uint16_t)w;
        if (reg == 0x210D) p->m7h = sext13(v << 8 | p->m7latch);
        if (reg == 0x210E) p->m7v = sext13(v << 8 | p->m7latch);
        p->bglatch = (uint8_t)v;
        if (reg <= 0x210E) p->m7latch = (uint8_t)v;
    } else if (reg >= 0x211B && reg <= 0x2120) {
        uint32_t w = v << 8 | p->m7latch;
        p->m7[reg - 0x211B] = reg >= 0x211F ? sext13(w) : (int16_t)w;
        p->m7latch = (uint8_t)v;
    } else if (reg == 0x2132) {
        if (v & 0x20) p->coldata[0] = v & 31;
        if (v & 0x40) p->coldata[1] = v & 31;
        if (v & 0x80) p->coldata[2] = v & 31;
    }
}
/* (I11, fescr.c) per frame buffer and map row of the mirror (32 words) the columns written since that
   buffer's last finished picture: a text typed in redraws only its columns (0: no front-end screen set up) */
uint32_t *fe_dirty;
#define FE_DIRTY_MARK(i) do { uint32_t *d_ = fe_dirty; if (d_) { uint32_t b_ = 1u << ((i) & 31); d_[(i) >> 5] |= b_; d_[128 + ((i) >> 5)] |= b_; } } while (0)
__attribute__((noinline)) static void fe_vdma(uint32_t w, uint32_t src, uint32_t n, int fixed)
{
    for (uint32_t k = 0; k < (n >> 1); k++) {
        uint32_t a = (w + k) & 0x7FFF;
        if (a < 0x7000) continue;
        uint32_t lo = mrd8((src + (fixed ? 0 : k * 2)) & 0xFFFFFF), hi = mrd8((src + (fixed ? 0 : k * 2 + 1)) & 0xFFFFFF);
        uint16_t nv = (uint16_t)(lo | hi << 8);
        if (fe_vm[a - 0x7000] != nv) { fe_vm[a - 0x7000] = nv; FE_TRK->rgen[(a - 0x7000) >> 6]++; FE_DIRTY_MARK(a - 0x7000); }
        FE_TRK->vmask |= 1u << ((a - 0x7000) >> 8);
    }
    FE_TRK->vgen++;
}
__attribute__((noinline)) static void fe_vwr(uint32_t a, uint32_t reg, uint32_t v)
{
    uint32_t i = a - 0x7000, o = fe_vm[i] == 0xFFFF ? 0 : fe_vm[i];   /* (0xFFFF: not written since lent) */
    uint16_t nv = reg & 1 ? (uint16_t)((o & 0xFF) | v << 8) : (uint16_t)((o & 0xFF00) | v);
    if (fe_vm[i] != nv) { fe_vm[i] = nv; FE_TRK->rgen[i >> 6]++; FE_DIRTY_MARK(i); }
    FE_TRK->vmask |= 1u << (i >> 8);
    FE_TRK->vgen++;
}

R65_HOT static void vram_dma(uint32_t src, uint32_t n, int fixed)
{
    /* word address vmadd, 2 bytes per word (DMA mode 1 to $2118/9, increment after the high byte) */
    uint32_t w = vmadd;
#ifdef VRAMLOG
    if (vram_log) { extern int printf(const char *, ...); printf("  vram dma %06X -> %04X n %X f %u fb %d\n", src, w, n, game_stats.frames, game_fblank);
        if (n == 0 || n == 0x10000) { printf("   ZP50:"); for (int i = 0x50; i < 0x60; i++) printf(" %02X", wram_lo[i]); printf(" 8AB %02X%02X\n", wram_lo[0x8AC], wram_lo[0x8AB]); } }
#endif
    if (w < 0x2000 && !(w & 15) && !(n & 31)) {
        uint32_t t = w >> 4, nt = n >> 5;
        const uint8_t *p = snes_ptr(src);
        uint32_t bank = src >> 16;
        if (fixed) {
            /* every tile = the source byte (DKC clears VRAM this way when it loads a level) */
            uint8_t b = *p;
            for (int i = 0; i < 32; i++) fill_tile[i] = b;
            for (uint32_t i = 0; i < nt && t + i < 512; i++) obj_set(t + i, fill_tile);
            spr_stats.tiles += nt;
            vmadd = (vmadd + (n >> 1)) & 0x7FFF;
            return;
        }
        if ((bank & 0xFE) == 0x7E || (!(bank & 0x40) && (src & 0xFFFF) < 0x2000)) {
            /* graphics the game unpacked into WRAM (a level (re)load): the buffer may be scratch
               memory, so use the level-start snapshot's VRAM (the same level's tiles) */
            for (uint32_t i = 0; i < nt && t + i < 512; i++) obj_set(t + i, SNAP_TILE(t + i));
            spr_stats.wram_dma++;
#ifndef __sh__
            {   extern char *getenv(const char *); static void *df; extern void *fopen(const char *, const char *);
                extern int fprintf(void *, const char *, ...);
                if (!df && getenv("DMALOG")) df = fopen(getenv("DMALOG"), "a");
                if (df) fprintf(df, "%06X %X W %04X\n", src & 0xFFFFFF, n, w);
            }
#endif
        } else {
            /* ROM: the blob holds only the pages in use, so a source can continue elsewhere after a
               1 KB page boundary - look every tile up */
            for (uint32_t i = 0; i < nt && t + i < 512; i++) obj_set(t + i, snes_ptr(src + i * 32));
#ifndef __sh__
            {   /* (host: DMALOG=file - the ROM ranges uploaded as OBJ graphics, sprite usage per world) */
                extern char *getenv(const char *); static void *df; extern void *fopen(const char *, const char *);
                extern int fprintf(void *, const char *, ...);
                if (!df && getenv("DMALOG")) df = fopen(getenv("DMALOG"), "a");
                if (df) fprintf(df, "%06X %X\n", src & 0xFFFFFF, n);
            }
#endif
        }
        spr_stats.tiles += nt;
    } else if (w < 0x2000) {
        spr_stats.odd_dma++;
#ifdef VRAMLOG
        { extern int printf(const char *, ...); printf("  ODD vram dma %06X -> %04X n %X\n", src, w, n); }
#endif
    }
    if (!fe_vm && fe_lend_hook && w + (n >> 1) > 0x7000) fe_lend_hook();
    if (fe_vm && w + (n >> 1) > 0x7000) fe_vdma(w, src, n, fixed);
    vmadd = (vmadd + (n >> 1)) & 0x7FFF;
}

R65_HOT static void cgram_byte(uint32_t v)
{
    if (!cg_phase) { cg_latch = v; cg_phase = 1; return; }
    cg_phase = 0;
    uint32_t c = cg_latch | (v & 0x7F) << 8;
    if (cgadd < 128) {
        /* BG colours: mirrored for the levels whose 32X CRAM 1-127 is the SNES CGRAM (main.c); colour 0
           of a palette is only used by the front-end screens (backdrop, 8 bpp) */
        uint16_t nv = (uint16_t)(c | 0x8000);
        if (spr_bgpal[cgadd] != nv) { spr_bgpal[cgadd] = nv; if (cgadd & 15) spr_bgpal_dirty |= 1u << (cgadd >> 4); }
    } else {
        uint32_t i = cgadd - 128;
        uint16_t nv = (uint16_t)(c | 0x8000);
        if (spr_pal[i] != nv) { spr_pal[i] = nv; spr_pal_dirty |= 1u << (i >> 4); }
    }
    cgadd = (cgadd + 1) & 0xFF;
}

#ifdef VRAM_SHADOW
/* host check: a byte-exact VRAM copy from every DMA / CPU write; spr_check() compares the OBJ tiles
   the visible OAM uses with our tile pointers */
uint8_t shv[0x10000];
static void shv_dma(uint32_t dmap, uint32_t bbus, uint32_t src, uint32_t n)
{
    uint32_t w = vmadd, mode = dmap & 7, fixed = (dmap & 0x08) != 0;
    if (n == 0) n = 0x10000;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t b = mrd8((src + (fixed ? 0 : k)) & 0xFFFFFF);
        uint32_t reg = bbus;
        if (mode == 1) reg = bbus + (k & 1);
        if (reg != 0x18 && reg != 0x19) continue;
        shv[((w & 0x7FFF) << 1) | (reg & 1)] = (uint8_t)b;
        if ((reg == 0x19) == ((vmain & 0x80) != 0)) { static const uint8_t st[4] = { 1, 32, 128, 128 }; w = (w + st[vmain & 3]) & 0x7FFF; }
    }
}
void spr_check(uint32_t frame)
{
    extern int printf(const char *, ...);
    const uint8_t *oam = &wram_lo[0x200], *hi = oam + 0x200;
    static int nrep; static uint32_t ncmp;
    if (frame == 1000) printf("SPRCHK compared %u tiles\n", ncmp);
    for (int i = 0; i < 128; i++) {
        const uint8_t *o = oam + i * 4;
        uint32_t h = hi[i >> 2] >> ((i & 3) * 2);
        int32_t y = o[1]; if (y >= 224) y -= 256;
        int sz = (h & 2) ? 16 : 8;
        if (y + sz <= 0 || y >= 224) continue;
        uint32_t t0 = o[2] | (o[3] & 1) << 8;
        for (int ty = 0; ty < sz / 8; ty++)
            for (int tx = 0; tx < sz / 8; tx++) {
                uint32_t t = (t0 & 0x100) | ((t0 + ty * 16) & 0xF0) | ((t0 + tx) & 0xF);
                ncmp++;
                if (spr_ck && nrep < 400000) {
                    for (int r = 0; r < 8; r++)
                        if (spr_ck[t * 8 + r] != ck_row(&shv[t * 32], r)) {
                            nrep++; printf("SPRCHK f %u oam %d tile %03X chunky row %d stale\n", frame, i, t, r); break;
                        }
                }
                if (memcmp(obj_tile[t], &shv[t * 32], 32) && nrep < 400000) {
                    nrep++;
                    printf("SPRCHK f %u oam %d tile %03X (x %d y %d) ptr %p differs\n", frame, i, t, o[0], y, (void *)obj_tile[t]);
                }
            }
    }
}
#endif
R65_HOT void game_dma_hook(uint32_t dmap, uint32_t bbus, uint32_t src, uint32_t n)
{
#ifdef VRAM_SHADOW
    if (!(dmap & 0x80) && (bbus == 0x18 || bbus == 0x19)) shv_dma(dmap, bbus, src, n);
#endif
#ifdef VRAMLOG
    if (vram_log && (bbus == 0x18 || bbus == 0x19) && (dmap & 7) != 1) { extern int printf(const char *, ...); printf("  other vram dma dmap %02X bbus %02X src %06X n %X vmadd %04X vmain %02X f %u\n", dmap, bbus, src, n, vmadd, vmain, game_stats.frames); }
#endif
    if (dmap & 0x80) return;                       /* B -> A */
    if (bbus == 0x18 && (dmap & 7) == 1) { vram_dma(src, n, (dmap & 0x08) != 0); return; }
#ifndef NOMODE0
    if ((bbus == 0x18 || bbus == 0x19) && (dmap & 7) == 0) {
        /* one register: low or high bytes only (the credits fill their map's attribute bytes this
           way) - as CPU writes (VMAIN increment, the front-end map mirror) */
        int step = (dmap & 0x08) ? 0 : (dmap & 0x10) ? -1 : 1;
        for (uint32_t k = 0; k < n; k++) game_ppu_hook(0x2100 | bbus, mrd8((src + (uint32_t)(step * (int)k)) & 0xFFFFFF));
        return;
    }
#endif
    if (bbus == 0x22) {
        for (uint32_t k = 0; k < n; k++) cgram_byte(mrd8((src + k) & 0xFFFFFF));
        return;
    }
}

R65_HOT void game_ppu_hook(uint32_t reg, uint32_t v)
{
#ifdef VRAMLOG
    if (vram_log && (reg == 0x2101 || (reg >= 0x2107 && reg <= 0x210C))) { extern int printf(const char *, ...); printf("  ppu %04X = %02X f %u\n", reg, v, game_stats.frames); }
#endif
#ifdef VRAMLOG
    if (vram_log && (reg == 0x2118 || reg == 0x2119)) { extern int printf(const char *, ...); printf("  ppu vram write %04X = %02X vmadd %04X vmain %02X f %u fb %d main %d\n", reg, v, vmadd, vmain, game_stats.frames, game_fblank, game_in_main_loop()); }
#endif
    if (reg == 0x2118 || reg == 0x2119) {
#ifdef VRAM_SHADOW
        shv[(vmadd << 1) | (reg & 1)] = (uint8_t)v;
#endif
        /* CPU VRAM writes (the world map's font is written this way): OBJ tiles go to a writable
           copy in spr_rwbuf (uncached alias: the slave draws from it), only while the level is not
           on screen - the buffer is borrowed from the level renderer */
        if (vmadd < 0x2000 && spr_rwbuf && (game_dark() || !game_in_main_loop())) {
            uint32_t t = vmadd >> 4;
            uint8_t *d = (uint8_t *)(((uint32_t)spr_rwbuf + t * 32) | 0x20000000);
            if (obj_tile[t] != d) {
                const uint8_t *src = obj_tile[t];
                for (int i = 0; i < 32; i++) d[i] = src[i];
                obj_tile[t] = d;
            }
            d[(vmadd & 7) * 2 + (vmadd & 8) * 2 + (reg & 1)] = (uint8_t)v;
            CK_MARK(t);
        }
        if (!fe_vm && fe_lend_hook && vmadd >= 0x7000) fe_lend_hook();
        if (fe_vm && vmadd >= 0x7000) fe_vwr(vmadd, reg, v);
        if ((reg == 0x2119) == ((vmain & 0x80) != 0)) {
            static const uint8_t st[4] = { 1, 32, 128, 128 };
            vmadd = (vmadd + st[vmain & 3]) & 0x7FFF;
        }
        return;
    }
    switch (reg) {
    case 0x2115: vmain = v; break;
    case 0x2116: vmadd = (vmadd & 0x7F00) | v; break;
    case 0x2117: vmadd = (vmadd & 0x00FF) | (v & 0x7F) << 8; break;
    case 0x2121: cgadd = v; cg_phase = 0; break;
    case 0x2122: cgram_byte(v); break;
    default: if ((reg >= 0x210D && reg <= 0x2120) || reg == 0x2132) fe_reg(reg, v); break;
    }
}

/* the uploads the game queued for the next NMI (graphics DMA list $170F, palette ring $1A8F):
   applied now so this frame's OAM is drawn with its own graphics (the NMI repeats them) */
/* the OAM entries that can be on screen, in drawing order (127 .. 0), built once by the master
   (its cache holds the OAM the game just wrote) for both CPUs' bands */
typedef struct { int16_t x, y; uint16_t tile; uint8_t attr, size; } spr_ent_t;
static spr_ent_t spr_list[128];
int32_t spr_xoff;
int32_t spr_fixed_x = 0x7FFF;        /* (I21c) Galleon's island: its x offset (0x7FFF: none) */
static volatile uint32_t spr_nlist;
R65_HOT static void spr_build_list(void)
{
    const uint8_t *oam = &wram_lo[0x200], *hi = oam + 0x200;
    spr_ent_t *d = spr_list;
    for (int i = 127; i >= 0; i--) {
        const uint8_t *o = oam + i * 4;
        uint32_t hb = hi[i >> 2], h;
        switch (i & 3) { case 0: h = hb; break; case 1: h = hb >> 2; break; case 2: h = hb >> 4; break; default: h = hb >> 6; }
        int32_t y = o[1];
        if (y >= 224) y -= 256;
        int32_t size = (h & 2) ? 16 : 8;
        if (y + size <= 0 || y >= SCREEN_H) continue;
        int32_t x = o[0] | (h & 1) << 8;
        if (x >= 320) x -= 512;     /* 9-bit x: 256..319 = the 32X's extra columns (widescreen patch) */
        /* (the view left of the game's camera at a level's right end); (I21c) the Galleon boss's distant
           island at a fixed offset (its shadow band on the Genesis plane: md update_scroll) */
        x += (spr_fixed_x != 0x7FFF && (o[2] & 0xF0) == 0x40 && (o[3] & 0x3F) == 0x2D) ? spr_fixed_x : spr_xoff;
        if (x <= -size || x >= SCREEN_W) continue;
        d->x = (int16_t)x; d->y = (int16_t)y; d->tile = (uint16_t)(o[2] | (o[3] & 1) << 8);
        d->attr = o[3]; d->size = (uint8_t)size;
        d++;
    }
    spr_nlist = (uint32_t)(d - spr_list);
}

/* (I17) spr_prepare in two parts, so the slave can take the second one in the normal mode while the
   master goes on with the picture's setup:
   spr_prepare_a (master): the queued graphics uploads (they change obj_tile, the master's state), the
   palette ring, and the list of tiles to convert (ck_dirty / ck_nlist reset here, by the master);
   spr_prepare_b (either CPU): the visible-sprite list and the conversions.  Both write only data the
   renderers read UNCACHED (spr_list / spr_nlist, spr_ck), so the master needs no cache purge after
   the slave did it; the slave purges its own cache before (it reads what the master just wrote). */
static uint16_t spr_cvq[CK_LMAX];
static volatile uint32_t spr_cvn;
R65_HOT void spr_prepare_a(void)
{
    for (int k = 0; k < 64; k++) {
        const uint8_t *e = &wram_lo[0x170F + k * 8];
        if (e[6] < 0x80) break;
        uint32_t save = vmadd;
        vmadd = (e[2] | e[3] << 8) & 0x7FFF;
#ifdef VRAM_SHADOW
        shv_dma(1, 0x18, (uint32_t)e[6] << 16 | e[4] | e[5] << 8, e[0] | e[1] << 8);
#endif
        vram_dma((uint32_t)e[6] << 16 | e[4] | e[5] << 8, e[0] | e[1] << 8, 0);
        vmadd = save;
    }
    {   /* the changed tiles (after the queued uploads above: a tile they change must be converted for
           THIS frame's OAM - test 8..12 converted first: stale tiles while animating) */
        uint32_t n = ck_nlist, q = 0;
#ifdef CKSTAT
        {   /* (test) conversions per frame and hits of a source-keyed memo (256 x direct mapped) */
            extern int printf(const char *, ...); static uint32_t nconv, nhit, nfr, nhit2; static const uint8_t *memo[256], *memo2[1024];
            uint32_t m = n <= CK_LMAX ? n : 512;
            for (uint32_t i = 0; i < m; i++) {
                uint32_t t = n <= CK_LMAX ? ck_list[i] : i;
                if (n > CK_LMAX && !ck_dirty[t]) continue;
                const uint8_t *p = obj_tile[t]; nconv++;
                uint32_t h = ((uint32_t)(uintptr_t)p >> 5) & 255, h2 = ((uint32_t)(uintptr_t)p >> 5) & 1023;
                if (memo[h] == p) nhit++; else memo[h] = p;
                if (memo2[h2] == p) nhit2++; else memo2[h2] = p;
            }
            if (++nfr % 1000 == 0) printf("CKSTAT frames %u conv %u (%.1f/frame) memo256 hits %.1f%% memo1024 %.1f%%\n", nfr, nconv, nconv / (double)nfr, 100.0 * nhit / nconv, 100.0 * nhit2 / nconv);
        }
#endif
        if (n <= CK_LMAX) {
            for (uint32_t i = 0; i < n; i++) {
                uint32_t t = ck_list[i];
                ck_dirty[t] = 0;
                spr_cvq[q++] = (uint16_t)t;
            }
        } else {
            for (uint32_t t = 0; t < 512; t++)
                if (ck_dirty[t]) { ck_dirty[t] = 0; if (spr_ck) ck_conv(t, obj_tile[t]); }   /* (rare: a level load) */
        }
        ck_nlist = 0;
        spr_cvn = q;
    }
    uint32_t rd = wram_lo[0x1ACF] & 15, wr = wram_lo[0x1AD1] & 15;
    while (rd != wr) {
        const uint8_t *e = &wram_lo[0x1A8F + rd * 4];
        uint32_t src = e[0] | e[1] << 8 | e[2] << 16;
        uint32_t save = cgadd;
        cgadd = e[3]; cg_phase = 0;
        for (int b = 0; b < 30; b++) cgram_byte(mrd8((src + b) & 0xFFFFFF));
        cgadd = save;
        rd = (rd + 1) & 15;
    }
}
R65_HOT void spr_prepare_b(void)
{
    spr_build_list();
    if (!spr_ck) obj_tile_snap();
    uint32_t n = spr_cvn;
    if (spr_ck)
        for (uint32_t i = 0; i < n; i++) {
            uint32_t t = spr_cvq[i];
            ck_conv(t, obj_tile[t]);
        }
    spr_cvn = 0;
}
R65_HOT void spr_prepare(void)
{
    spr_prepare_a();
    spr_prepare_b();
}

/* ---- drawing (SDRAM: runs on both CPUs) */
#define SDRAM_CODE __attribute__((section(".sdtext"), aligned(16)))
static uint16_t pairlut[8][256];        /* palette p: two pixel nibbles -> framebuffer word (0 = clear) */
static const uint32_t spr_bit[32] = {
    1u << 0, 1u << 1, 1u << 2, 1u << 3, 1u << 4, 1u << 5, 1u << 6, 1u << 7, 1u << 8, 1u << 9, 1u << 10,
    1u << 11, 1u << 12, 1u << 13, 1u << 14, 1u << 15, 1u << 16, 1u << 17, 1u << 18, 1u << 19, 1u << 20,
    1u << 21, 1u << 22, 1u << 23, 1u << 24, 1u << 25, 1u << 26, 1u << 27, 1u << 28, 1u << 29, 1u << 30, 1u << 31 };
#define CV_P    352
#define CV_XOFF 64
#define FB_BASE 0x200

void spr_tables(void)
{
    for (int b = 0; b < 256; b++) {
        uint32_t v = 0, r = 0;
        for (int i = 0; i < 8; i++) if (b >> i & 1) { v |= 1u << (4 * i); r |= 1u << (4 * (7 - i)); }
        spread[b] = v; (void)r;
    }
    for (int p = 0; p < 8; p++)
        for (int b = 0; b < 256; b++) {
            uint32_t hi = b >> 4, lo = b & 15, base = 128 + p * 16;
            pairlut[p][b] = (uint16_t)((hi ? (base + hi) << 8 : 0) | (lo ? base + lo : 0));
        }
}

/* one 8x8 tile, rows inside [y0, y1).  The overwrite region ignores zero bytes, so whole words /
   longs are written (transparent pixels are 0); the canvas line wraps after 256 lines. */
#define OVRB ((volatile uint8_t *)OVR16)
#define W16(o, v) (*(volatile uint16_t *)(OVRB + (o)) = (uint16_t)(v))
#define W32(o, v) (*(volatile uint32_t *)(OVRB + (o)) = (v))
/* (G1) OBJ priority 0/1: the SNES draws these behind BG1 (the Kongs walking into a bonus room door
   or an exit): only the pixels where the canvas (BG1) is transparent - read back from the framebuffer.
   Rare (a few frames at doors), so a plain per-pixel loop. */
static void tile8_behind(const uint32_t *rows, int32_t sx, int32_t sy, const uint16_t *L, int hf, int vf,
                         int32_t y0, int32_t y1, int32_t gx, int32_t gy, int prio2);
SDRAM_CODE static void tile8(const uint32_t *rows, int32_t sx, int32_t sy, const uint16_t *L, int hf, int vf,
                             int32_t y0, int32_t y1, int32_t gx, int32_t gy);
/* BG1 tile priorities (main.c, per level): grid of vchunks + their 16-bit priority masks; OBJ
   priority 2 goes behind the opaque pixels of BG1 high-priority tiles (exit huts, bonus doors) */
const uint16_t *volatile spr_prio_grid, *volatile spr_prio_mask;
volatile int32_t spr_prio_gw, spr_prio_gh;
/* (perf) does the 8x8 screen square at (px, py) touch a BG1 high-priority tile? */
static int prio_any(int32_t px, int32_t py, int32_t gx, int32_t gy)
{
    const uint16_t *pg = *(const uint16_t *const *)((uint32_t)&spr_prio_grid | 0x20000000);
    const uint16_t *pm = *(const uint16_t *const *)((uint32_t)&spr_prio_mask | 0x20000000);
    int32_t gw = *(volatile int32_t *)((uint32_t)&spr_prio_gw | 0x20000000);
    int32_t gh = *(volatile int32_t *)((uint32_t)&spr_prio_gh | 0x20000000);
    if (!pm || !pg) return 0;
    int32_t tx0 = (gx + px) >> 3, tx1 = (gx + px + 7) >> 3, ty0 = (gy + py) >> 3, ty1 = (gy + py + 7) >> 3;
    for (int32_t ty = ty0; ty <= ty1; ty++)
        for (int32_t tx = tx0; tx <= tx1; tx++) {
            int32_t cx = tx >> 2, cy = ty >> 2;
            if (cx < 0 || cy < 0 || cx >= gw || cy >= gh) continue;
            if (pm[pg[cy * gw + cx]] & spr_bit[(ty & 3) * 4 + (tx & 3)]) return 1;
        }
    return 0;
}
/* one OBJ drawn behind BG1 (ROM code: SDRAM is full).  (perf) Priority 2 (most sprites in levels
   with BG1 high-priority tiles - every sprite in Very Gnawty's arena): only its 8x8 tiles that touch
   such a tile take the per-pixel path, the others the normal one. */
__attribute__((noinline)) static void obj_behind(const spr_ent_t *o, int32_t y0, int32_t y1, int32_t gx, int32_t gy,
                                                 const uint32_t *ck, const uint8_t *const *tiles)
{
    uint32_t cbuf[8];
    int32_t x = o->x, y = o->y, size = o->size;
    uint32_t attr = o->attr, tile = o->tile;
    const uint16_t *L = pairlut[(attr >> 1) & 7];
    int hf = attr & 0x40, vf = attr & 0x80, p2 = (attr & 0x20) != 0;
    for (int ty = 0; ty < size / 8; ty++) {
        int32_t dy = vf ? (size - 8) - ty * 8 : ty * 8;
        if (y + dy + 8 <= y0 || y + dy >= y1) continue;
        for (int tx = 0; tx < size / 8; tx++) {
            uint32_t t = (tile & 0x100) | ((tile + ty * 16) & 0xF0) | ((tile + tx) & 0x0F);
            int32_t dx = hf ? (size - 8) - tx * 8 : tx * 8;
            if (x + dx + 8 <= 0 || x + dx >= SCREEN_W) continue;
            const uint32_t *rows;
            if (ck) rows = (const uint32_t *)((uint32_t)(ck + t * 8) | 0x20000000);
            else { for (int r = 0; r < 8; r++) cbuf[r] = ck_row(tiles[t], r); rows = cbuf; }
            if (p2 && !prio_any(x + dx, y + dy, gx, gy)) tile8(rows, x + dx, y + dy, L, hf, vf, y0, y1, gx, gy);
            else tile8_behind(rows, x + dx, y + dy, L, hf, vf, y0, y1, gx, gy, p2);
        }
    }
}

static void tile8_behind(const uint32_t *rows, int32_t sx, int32_t sy, const uint16_t *L, int hf, int vf,
                         int32_t y0, int32_t y1, int32_t gx, int32_t gy, int prio2)
{
    const uint16_t *pg = 0, *pm = 0;
    int32_t gw = 0, gh = 0;
    if (prio2) {
        pg = *(const uint16_t *const *)((uint32_t)&spr_prio_grid | 0x20000000);
        pm = *(const uint16_t *const *)((uint32_t)&spr_prio_mask | 0x20000000);
        gw = *(volatile int32_t *)((uint32_t)&spr_prio_gw | 0x20000000);
        gh = *(volatile int32_t *)((uint32_t)&spr_prio_gh | 0x20000000);
    }
    for (int32_t r = 0; r < 8; r++) {
        int32_t y = sy + r;
        if (y < y0 || y >= y1) continue;
        uint32_t w = rows[vf ? 7 - r : r];
        if (!w) continue;
        if (hf) { w = __builtin_bswap32(w); w = ((w >> 4) & 0x0F0F0F0Fu) | ((w & 0x0F0F0F0Fu) << 4); }
        uint32_t line = (uint32_t)(gy + y) & 255;
        uint32_t a = (uint32_t)(FB_BASE + gx + CV_XOFF) + line * CV_P;
        int32_t ty = (gy + y) >> 3, cy = ty >> 2, lastc = 0x7FFFFFFF, hi = 0;
        for (int32_t c = 0; c < 8; c++, w <<= 4) {         /* (pixel c in the top nibble) */
            int32_t x = sx + c;
            uint32_t nib = w >> 28;
            if (!nib || x < 0 || x >= SCREEN_W) continue;
            volatile uint8_t *fb = (volatile uint8_t *)(0x24000000 + a + x);
            if (prio2) {                               /* only behind BG1 high-priority tiles */
                int32_t tx = (gx + x) >> 3;
                if (tx != lastc) {
                    int32_t cx = tx >> 2;
                    lastc = tx;
                    hi = !(cx < 0 || cy < 0 || cx >= gw || cy >= gh) && (pm[pg[cy * gw + cx]] & spr_bit[(ty & 3) * 4 + (tx & 3)]);
                }
                if (!hi) goto draw;
            }
            if (*fb) continue;                         /* BG1 (or a sprite drawn before) is there */
        draw:;
            uint32_t v = L[nib << 4];                  /* (pair LUT: high nibble = the left pixel) */
            *(volatile uint8_t *)(OVRB + a + x) = (uint8_t)(v >> 8);
        }
    }
}

SDRAM_CODE static void tile8(const uint32_t *rows, int32_t sx, int32_t sy, const uint16_t *L, int hf, int vf,
                             int32_t y0, int32_t y1, int32_t gx, int32_t gy)
{
    int32_t r0 = 0, r1 = 8;
    if (sy < y0) r0 = y0 - sy;
    if (sy + 8 > y1) r1 = y1 - sy;
    if (r0 >= r1) return;
    uint32_t line = (uint32_t)(gy + sy + r0) & 255;
    uint32_t a = (uint32_t)(FB_BASE + gx + CV_XOFF + sx) + line * CV_P;
    const uint32_t *tp = rows + (vf ? 7 - r0 : r0);
    int32_t ts = vf ? -1 : 1;
    int32_t n = r1 - r0;
    /* a row: 8 pixel nibbles, pixel 0 on top (h flip: reversed) */
#define ROWW ({ uint32_t _w = *tp; if (hf) { _w = __builtin_bswap32(_w); _w = ((_w >> 4) & 0x0F0F0F0Fu) | ((_w & 0x0F0F0F0Fu) << 4); } _w; })
#define NEXT do { tp += ts; a += CV_P; if (++line == 256) { line = 0; a -= 256 * CV_P; } } while (0)
    if (sx < 0 || sx > SCREEN_W - 8) {
        /* left / right edge: per pair with clipping */
        for (; n > 0; n--, ({ NEXT; })) {
            uint32_t w = ROWW;
            if (!w) continue;
            int odd = a & 1;
            uint32_t m = odd ? w >> 4 : w;
            for (int k = 0; k < 4 + odd; k++) {
                int32_t x = sx - odd + 2 * k;
                uint32_t nn = k < 4 ? (m >> (24 - 8 * k)) & 0xFF : (w & 15) << 4;
                uint32_t v = L[nn];
                if (!v) continue;
                if (x < 0 || x + 1 >= SCREEN_W) {
                    if (x + 1 == 0) v &= 0x00FF; else if (x + 1 == SCREEN_W) v &= 0xFF00; else continue;
                    if (!v) continue;
                }
                W16(a - odd + 2 * k, v);
            }
        }
        return;
    }
    switch (a & 3) {
    case 0:
        for (; n > 0; n--, ({ NEXT; })) {
            uint32_t w = ROWW;
            if (!w) continue;
            if (w >> 16) W32(a, (uint32_t)L[w >> 24] << 16 | L[(w >> 16) & 255]);     /* (skip transparent words: */
            if (w & 0xFFFF) W32(a + 4, (uint32_t)L[(w >> 8) & 255] << 16 | L[w & 255]);   /* FB writes are the cost) */
        }
        break;
    case 2:
        for (; n > 0; n--, ({ NEXT; })) {
            uint32_t w = ROWW;
            if (!w) continue;
            if (w >> 24) W16(a, L[w >> 24]);
            if ((w >> 8) & 0xFFFF) W32(a + 2, (uint32_t)L[(w >> 16) & 255] << 16 | L[(w >> 8) & 255]);
            if (w & 255) W16(a + 6, L[w & 255]);
        }
        break;
    case 1:      /* a - 1 is long aligned: pairs shifted by one pixel */
        for (; n > 0; n--, ({ NEXT; })) {
            uint32_t w = ROWW;
            if (!w) continue;
            uint32_t m = w >> 4;
            if (m >> 16) W32(a - 1, (uint32_t)L[m >> 24] << 16 | L[(m >> 16) & 255]);
            if (m & 0xFFFF) W32(a + 3, (uint32_t)L[(m >> 8) & 255] << 16 | L[m & 255]);
            if (w & 15) W16(a + 7, L[(w & 15) << 4]);
        }
        break;
    default:     /* 3: a - 1 is word aligned */
        for (; n > 0; n--, ({ NEXT; })) {
            uint32_t w = ROWW;
            if (!w) continue;
            uint32_t m = w >> 4;
            if (m >> 24) W16(a - 1, L[m >> 24]);
            if ((m >> 8) & 0xFFFF) W32(a + 1, (uint32_t)L[(m >> 16) & 255] << 16 | L[(m >> 8) & 255]);
            if ((m & 255) | (w & 15)) W32(a + 5, (uint32_t)L[m & 255] << 16 | L[(w & 15) << 4]);
        }
        break;
    }
#undef ROWW
#undef NEXT
}

/* water ripple (BG1 bands shown 1 px right and 1 line down, see main.c write_line_table_cv): the
   sprites share BG1's canvas, so each sprite row is written where its screen line shows the canvas
   (+1 line, +1 px in the shifted bands) - the sprites stay still on screen like on the SNES.  (Where a
   band ends two screen lines show one canvas line: one sprite row there is lost, as BG1's.) */
const uint8_t *spr_rip_pat;              /* 64-line band pattern (0 / 1), or 0: no ripple */
int32_t spr_rip_phase;
SDRAM_CODE static void tile8r(const uint32_t *rows, int32_t sx, int32_t sy, const uint16_t *L, int hf, int vf,
                              int32_t y0, int32_t y1, int32_t gx, int32_t gy, const uint8_t *rp, int32_t ph)
{
    int32_t r0 = 0, r1 = 8;
    if (sy < y0) r0 = y0 - sy;
    if (sy + 8 > y1) r1 = y1 - sy;
    for (int32_t r = r0; r < r1; r++) {
        int32_t y = sy + r;
        uint32_t w = rows[vf ? 7 - r : r];
        if (!w) continue;
        if (hf) { w = __builtin_bswap32(w); w = ((w >> 4) & 0x0F0F0F0Fu) | ((w & 0x0F0F0F0Fu) << 4); }
        uint32_t dd = rp[(y - ph) & 63];
        uint32_t line = (uint32_t)(gy + y + (int32_t)dd) & 255;
        uint32_t a = (uint32_t)(FB_BASE + gx + CV_XOFF + sx + (int32_t)dd) + line * CV_P;
        int odd = a & 1;
        uint32_t m = odd ? w >> 4 : w;
        for (int k = 0; k < 4 + odd; k++) {
            int32_t x = sx - odd + 2 * k;
            uint32_t nn = k < 4 ? (m >> (24 - 8 * k)) & 0xFF : (w & 15) << 4;
            uint32_t v = L[nn];
            if (!v) continue;
            if (x < 0 || x + 1 >= SCREEN_W) {
                if (x + 1 == 0) v &= 0x00FF; else if (x + 1 == SCREEN_W) v &= 0xFF00; else continue;
                if (!v) continue;
            }
            W16(a - odd + 2 * k, v);
        }
    }
}

/* temple levels: the millstone (SNES BG2, one 256 x 112 picture placed per frame by CODE_809278)
   as 8x8 pseudo-sprites under the OAM ones (main.c sets these per game frame; ms_on = 0: none) */
static uint16_t ms_lut[256];
volatile int32_t spr_ms_on, spr_ms_x0, spr_ms_y0;
const uint16_t *volatile spr_ms_cells;      /* [448]: tile | hflip << 14 | vflip << 15, 0xFFFF empty */
const uint32_t *spr_ms_tiles;               /* chunky rows, 8 per tile */
int32_t spr_ms_c0, spr_ms_r0, spr_ms_c1, spr_ms_r1;
const uint32_t *spr_ms_opq;                 /* [4][14]: fully opaque cells per tilemap (bit = column) */
const uint8_t *spr_ms_span;                 /* [4][14][2]: non-empty columns [first, end) per tilemap row */
volatile int32_t spr_ms_k;                  /* the tilemap shown this frame */
volatile int32_t spr_ms_qpos[2][3];         /* per buffer: canvas x, y of the picture drawn there, on */
volatile int32_t spr_ms_cq[4];              /* this frame's x, y, on, tilemap (set by the master before the bands) */
volatile uint32_t spr_ms_gen;               /* bumped when the picture data is (re)copied */
/* canvas tile (tx, ty) fully covered by opaque millstone pixels with the picture at canvas (qx, qy):
   its restore can be skipped (the picture is drawn over it before any sprite) */
/* the picture data (SDRAM copy + ms_lut) was (re)written at a level load: each CPU drops its cache
   once (spr_ms_gen), after that the data is read cached (I18: ms_lut and the opaque masks were read
   uncached - 4 uncached SDRAM reads per 8-pixel row made the picture cost a third of a frame) */
void spr_ms_sync(void)
{
#ifdef __sh__
    static uint32_t seen[2];
    uint32_t sp, gen = *(volatile uint32_t *)((uint32_t)&spr_ms_gen | 0x20000000);
    __asm__ volatile ("mov r15,%0" : "=r"(sp));
    uint32_t *sn = (uint32_t *)((uint32_t)&seen[sp >= 0x0603F800] | 0x20000000);
    if (*sn != gen) { *sn = gen; *(volatile uint8_t *)0xFFFFFE92 |= 0x10; }
#endif
}
int spr_ms_cover(int32_t qx, int32_t qy, int32_t k, int32_t tx, int32_t ty)
{                                                           /* (the caller ran spr_ms_sync) */
    const uint32_t *opq = *(const uint32_t *const *)((uint32_t)&spr_ms_opq | 0x20000000);
    if (opq) opq += k * 14;
    int32_t dx = tx * 8 - qx, dy = ty * 8 - qy;
    int32_t c0 = dx >> 3, c1 = (dx + 7) >> 3, r0 = dy >> 3, r1 = (dy + 7) >> 3;
    if (!opq || c0 < 0 || c1 > 31 || r0 < 0 || r1 > 13) return 0;
    uint32_t m = (1u << c0) | (1u << c1);
    return (opq[r0] & m) == m && (opq[r1] & m) == m;
}
void spr_ms_palette(uint32_t pal)
{
    for (int b = 0; b < 256; b++) {
        uint32_t hi = b >> 4, lo = b & 15, base = pal * 16;
        ms_lut[b] = (uint16_t)((hi ? (base + hi) << 8 : 0) | (lo ? base + lo : 0));
    }
}
/* (I18) a fully opaque millstone cell inside the screen columns: tile8 without the transparency tests
   (the h flip decided once per cell, not per row) */
#define MS_OPQ_ROWS(FLIP) do {                                                                          \
    switch (a & 3) {                                                                                   \
    case 0: for (; n > 0; n--) { uint32_t w = FLIP(*tp); tp += ts;                                      \
                W32(a, (uint32_t)L[w >> 24] << 16 | L[(w >> 16) & 255]);                               \
                W32(a + 4, (uint32_t)L[(w >> 8) & 255] << 16 | L[w & 255]);                            \
                a += CV_P; if (++line == 256) { line = 0; a -= 256 * CV_P; } } break;                   \
    case 2: for (; n > 0; n--) { uint32_t w = FLIP(*tp); tp += ts;                                      \
                W16(a, L[w >> 24]);                                                                    \
                W32(a + 2, (uint32_t)L[(w >> 16) & 255] << 16 | L[(w >> 8) & 255]);                    \
                W16(a + 6, L[w & 255]);                                                                \
                a += CV_P; if (++line == 256) { line = 0; a -= 256 * CV_P; } } break;                   \
    case 1: for (; n > 0; n--) { uint32_t w = FLIP(*tp); tp += ts; uint32_t m = w >> 4;                 \
                W32(a - 1, (uint32_t)L[m >> 24] << 16 | L[(m >> 16) & 255]);                           \
                W32(a + 3, (uint32_t)L[(m >> 8) & 255] << 16 | L[m & 255]);                            \
                W16(a + 7, L[(w & 15) << 4]);                                                          \
                a += CV_P; if (++line == 256) { line = 0; a -= 256 * CV_P; } } break;                   \
    default: for (; n > 0; n--) { uint32_t w = FLIP(*tp); tp += ts; uint32_t m = w >> 4;                \
                W16(a - 1, L[m >> 24]);                                                                \
                W32(a + 1, (uint32_t)L[(m >> 16) & 255] << 16 | L[(m >> 8) & 255]);                    \
                W32(a + 5, (uint32_t)L[m & 255] << 16 | L[(w & 15) << 4]);                             \
                a += CV_P; if (++line == 256) { line = 0; a -= 256 * CV_P; } } break;                   \
    } } while (0)
#define MS_NOFLIP(w) (w)
#define MS_HFLIP(w) ({ uint32_t _w = __builtin_bswap32(w); ((_w >> 4) & 0x0F0F0F0Fu) | ((_w & 0x0F0F0F0Fu) << 4); })
SDRAM_CODE static void ms_tile8_opq(const uint32_t *rows, int32_t sx, int32_t sy, const uint16_t *L, int hf, int vf,
                                    int32_t y0, int32_t y1, int32_t gx, int32_t gy)
{
    int32_t r0 = 0, r1 = 8;
    if (sy < y0) r0 = y0 - sy;
    if (sy + 8 > y1) r1 = y1 - sy;
    if (r0 >= r1) return;
    uint32_t line = (uint32_t)(gy + sy + r0) & 255;
    uint32_t a = (uint32_t)(FB_BASE + gx + CV_XOFF + sx) + line * CV_P;
    const uint32_t *tp = rows + (vf ? 7 - r0 : r0);
    int32_t ts = vf ? -1 : 1;
    int32_t n = r1 - r0;
    if (hf) MS_OPQ_ROWS(MS_HFLIP);
    else MS_OPQ_ROWS(MS_NOFLIP);
}

SDRAM_CODE static void spr_ms_draw(int32_t y0, int32_t y1, int bi, int32_t gx, int32_t gy, uint32_t (*lm)[2], uint32_t *rowsp)
{
#define MSV(v) (*(volatile __typeof__(v) *)((uint32_t)&(v) | 0x20000000))
    volatile int32_t *qp = (volatile int32_t *)((uint32_t)&spr_ms_qpos[bi][0] | 0x20000000);
    if (!MSV(spr_ms_on)) { qp[2] = 0; return; }
    spr_ms_sync();
    int32_t x0 = MSV(spr_ms_x0), my0 = MSV(spr_ms_y0);
    const uint16_t *cells = (const uint16_t *)MSV(spr_ms_cells);
    const uint32_t *tiles = MSV(spr_ms_tiles);
    int32_t c0 = MSV(spr_ms_c0), c1 = MSV(spr_ms_c1), r0 = MSV(spr_ms_r0), r1 = MSV(spr_ms_r1);
    const uint16_t *L = ms_lut;
    int32_t xa = x0 + c0 * 8, xb = x0 + c1 * 8, ya = my0 + r0 * 8, yb = my0 + r1 * 8;
    if (xa < 0) xa = 0;
    if (xb > SCREEN_W) xb = SCREEN_W;
    if (ya < y0) ya = y0;
    if (yb > y1) yb = y1;
    qp[0] = gx + x0; qp[1] = gy + my0; qp[2] = 1;
    if (xa >= xb || ya >= yb) return;
    const uint32_t *opq = MSV(spr_ms_opq) + MSV(spr_ms_k) * 14;
    for (int32_t r = r0; r < r1; r++) {
        int32_t y = my0 + r * 8;
        if (y + 8 <= y0 || y >= y1) continue;
        uint32_t om = opq[r];
        for (int32_t c = c0; c < c1; c++) {
            int32_t x = x0 + c * 8;
            if (x + 8 <= 0 || x >= SCREEN_W) continue;
            uint32_t e = cells[r * 32 + c];
            if (e == 0xFFFF) continue;
#ifndef MSNODRAW
            if ((om >> c & 1) && x >= 0 && x <= SCREEN_W - 8)
                ms_tile8_opq(tiles + (e & 0x3FFF) * 8, x, y, L, e & 0x4000, e & 0x8000, y0, y1, gx, gy);
            else
                tile8(tiles + (e & 0x3FFF) * 8, x, y, L, e & 0x4000, e & 0x8000, y0, y1, gx, gy);
#endif
        }
    }
#ifdef MSNOMARK
    return;
#endif
    /* marks: per picture row, its non-empty columns (both canvas tile rows the cell row touches) */
    uint32_t rows = *rowsp;
    const uint8_t *sp = (const uint8_t *)((uint32_t)MSV(spr_ms_span) | 0x20000000) + MSV(spr_ms_k) * 28;
    for (int32_t r = r0; r < r1; r++) {
        int32_t cy0 = my0 + r * 8, cy1 = cy0 + 8;
        if (cy0 < ya) cy0 = ya;
        if (cy1 > yb) cy1 = yb;
        int32_t cx0 = x0 + sp[r * 2] * 8, cx1 = x0 + sp[r * 2 + 1] * 8;
        if (cx0 < xa) cx0 = xa;
        if (cx1 > xb) cx1 = xb;
        if (cy0 >= cy1 || cx0 >= cx1) continue;
        for (int32_t ty = (gy + cy0) >> 3; ty <= (gy + cy1 - 1) >> 3; ty++) {
            uint32_t q = ty & 31, *mr = lm[q];
            if (!(rows & spr_bit[q])) { rows |= spr_bit[q]; mr[0] = mr[1] = 0; }
            for (int32_t tx = (gx + cx0) >> 3; tx <= (gx + cx1 - 1) >> 3; tx++)
                mr[(tx >> 5) & 1] |= spr_bit[tx & 31];
        }
    }
    *rowsp = rows;
#undef MSV
}

/* draw the OAM sprites on screen lines [y0, y1) (index 127 first, 0 on top) and mark the canvas
   tiles they cover there for buffer bi (canvas tile = (gx + x) >> 3, (gy + y) >> 3) */
SDRAM_CODE void spr_draw_band(int32_t y0, int32_t y1, int bi, int32_t gx, int32_t gy)
{
    const spr_ent_t *ent = (const spr_ent_t *)((uint32_t)spr_list | 0x20000000);
    uint32_t ne = *(volatile uint32_t *)((uint32_t)&spr_nlist | 0x20000000);
    const uint8_t *const *tiles = (const uint8_t *const *)((uint32_t)obj_tile_dr | 0x20000000);
    const uint32_t *ck = *(uint32_t *const *)((uint32_t)&spr_ck | 0x20000000);
    uint32_t cbuf[8];
#define TROWS(t) (ck ? (const uint32_t *)((uint32_t)(ck + (t) * 8) | 0x20000000) : ({ const uint8_t *_p = tiles[t]; for (int _r = 0; _r < 8; _r++) cbuf[_r] = ck_row(_p, _r); (const uint32_t *)cbuf; }))
    uint32_t (*mk)[2] = MARKS_UNCACHED(&spr_marks[bi][0][0]);
    uint32_t lm[32][2], rows = 0;          /* marks gathered here (uncached read-modify-writes are slow) */
    const uint8_t *rp = *(const uint8_t *const *)((uint32_t)&spr_rip_pat | 0x20000000);
    int32_t rph = *(volatile int32_t *)((uint32_t)&spr_rip_phase | 0x20000000), rx = rp ? 1 : 0;
    uint32_t n = 0;
    spr_ms_draw(y0, y1, bi, gx, gy, lm, &rows);
    for (uint32_t i = 0; i < ne; i++) {
        const spr_ent_t *o = ent + i;
        int32_t y = o->y, size = o->size;
        if (y + size <= y0 || y >= y1) continue;
        int32_t x = o->x;
        uint32_t attr = o->attr, tile = o->tile;
        const uint16_t *L = pairlut[(attr >> 1) & 7];
        int hf = attr & 0x40, vf = attr & 0x80;
        n++;
        if (rp) {
            for (int ty = 0; ty < size / 8; ty++) {
                int32_t dy = vf ? (size - 8) - ty * 8 : ty * 8;
                if (y + dy + 8 <= y0 || y + dy >= y1) continue;
                for (int tx = 0; tx < size / 8; tx++) {
                    uint32_t t = (tile & 0x100) | ((tile + ty * 16) & 0xF0) | ((tile + tx) & 0x0F);
                    int32_t dx = hf ? (size - 8) - tx * 8 : tx * 8;
                    if (x + dx + 8 <= 0 || x + dx >= SCREEN_W) continue;
                    tile8r(TROWS(t), x + dx, y + dy, L, hf, vf, y0, y1, gx, gy, rp, rph);
                }
            }
        } else if (!*(volatile int *)((uint32_t)&spr_front_all | 0x20000000) && (!(attr & 0x20) || (!(attr & 0x10) && *(const uint16_t *const *)((uint32_t)&spr_prio_mask | 0x20000000)))) {
            /* priority 0/1: behind BG1; 2: behind BG1's high-priority tiles (levels that have some) */
            obj_behind(o, y0, y1, gx, gy, ck, tiles);
        } else if (size == 8) {
            tile8(TROWS(tile), x, y, L, hf, vf, y0, y1, gx, gy);
        } else {
            for (int ty = 0; ty < 2; ty++) {
                int32_t dy = vf ? 8 - ty * 8 : ty * 8;
                if (y + dy + 8 <= y0 || y + dy >= y1) continue;
                for (int tx = 0; tx < 2; tx++) {
                    uint32_t t = (tile & 0x100) | ((tile + ty * 16) & 0xF0) | ((tile + tx) & 0x0F);
                    int32_t dx = hf ? 8 - tx * 8 : tx * 8;
                    if (x + dx + 8 <= 0 || x + dx >= SCREEN_W) continue;
                    tile8(TROWS(t), x + dx, y + dy, L, hf, vf, y0, y1, gx, gy);
                }
            }
        }
        int32_t xa = x < 0 ? 0 : x, xb = x + size > SCREEN_W ? SCREEN_W : x + size;
        int32_t ya = y < y0 ? y0 : y, yb = y + size > y1 ? y1 : y + size;
        xb += rx;                          /* (ripple: drawn up to 1 px right; the line below a
                                              marked row is restored too - main.c cv_band) */
        for (int32_t ty = (gy + ya) >> 3; ty <= (gy + yb - 1) >> 3; ty++) {
            uint32_t r = ty & 31, *mr = lm[r];
            if (!(rows & spr_bit[r])) { rows |= spr_bit[r]; mr[0] = mr[1] = 0; }
            for (int32_t tx = (gx + xa) >> 3; tx <= (gx + xb - 1) >> 3; tx++)
                mr[(tx >> 5) & 1] |= spr_bit[tx & 31];
        }
    }
    for (int r = 0; rows; r++, rows >>= 1)
        if (rows & 1) { mk[r][0] |= lm[r][0]; mk[r][1] |= lm[r][1]; }
    spr_stats.drawn = n;
}

/* (I11, front-end screens: the master draws lines [0, split), the slave the rest) the line - a multiple of 8 -
   with about half of the sprites' 8x8 cells above it (0: no sprites) */
int spr_balance_split(void)
{
    const spr_ent_t *ent = (const spr_ent_t *)((uint32_t)spr_list | 0x20000000);
    uint32_t ne = *(volatile uint32_t *)((uint32_t)&spr_nlist | 0x20000000);
    uint16_t w[SCREEN_H / 8];
    uint32_t tot = 0;
    for (int r = 0; r < SCREEN_H / 8; r++) w[r] = 0;
    for (uint32_t i = 0; i < ne; i++) {
        int32_t y = ent[i].y, size = ent[i].size, x = ent[i].x;
        if (x + size <= 0 || x >= 256) continue;
        for (int32_t r = (y < 0 ? 0 : y) >> 3; r <= (y + size - 1) >> 3 && r < SCREEN_H / 8; r++) { w[r] += (uint16_t)(size >> 3); tot += size >> 3; }
    }
    if (!tot) return 0;
    uint32_t acc = 0;
    for (int r = 0; r < SCREEN_H / 8; r++) {
        acc += w[r];
        if (acc * 2 >= tot) return (r + 1) * 8;
    }
    return SCREEN_H;
}

/* (I3) Misty Mine's fog canvas: each 4-line band of the screen shows the canvas at its own offset (the
   fog's waves), so sprites go through the line table lt of the buffer drawn (screen line y = canvas
   bytes from lt[y] * 2): all sprites in front, byte writes through the overwrite region.  Their
   screen rectangles (x0, x1, y0, y1, clipped) -> r (up to maxr; -1: more than that) */
int spr_draw_lt(const volatile uint16_t *lt, int16_t (*r)[4], int maxr)
{
    const spr_ent_t *ent = (const spr_ent_t *)((uint32_t)spr_list | 0x20000000);
    uint32_t ne = *(volatile uint32_t *)((uint32_t)&spr_nlist | 0x20000000);
    const uint8_t *const *tiles = (const uint8_t *const *)((uint32_t)obj_tile_dr | 0x20000000);
    const uint32_t *ck = *(uint32_t *const *)((uint32_t)&spr_ck | 0x20000000);
    uint32_t cbuf[8];
    int nr = 0;
    for (uint32_t i = 0; i < ne; i++) {
        const spr_ent_t *o = ent + i;
        int32_t x = o->x, y = o->y, size = o->size;
        if (y + size <= 0 || y >= SCREEN_H || x + size <= 0 || x >= SCREEN_W) continue;
        uint32_t attr = o->attr, tile = o->tile;
        const uint16_t *L = pairlut[(attr >> 1) & 7];
        int hf = attr & 0x40, vf = attr & 0x80;
        for (int ty = 0; ty < size / 8; ty++) {
            int32_t dy = vf ? (size - 8) - ty * 8 : ty * 8;
            for (int tx = 0; tx < size / 8; tx++) {
                uint32_t t = (tile & 0x100) | ((tile + ty * 16) & 0xF0) | ((tile + tx) & 0x0F);
                int32_t dx = hf ? (size - 8) - tx * 8 : tx * 8;
                int32_t sx = x + dx, sy = y + dy;
                if (sx + 8 <= 0 || sx >= SCREEN_W || sy + 8 <= 0 || sy >= SCREEN_H) continue;
                const uint32_t *rows = TROWS(t);
                for (int rr = 0; rr < 8; rr++) {
                    int32_t yy = sy + rr;
                    if (yy < 0 || yy >= SCREEN_H) continue;
                    uint32_t w = rows[vf ? 7 - rr : rr];
                    if (!w) continue;
                    if (hf) { w = __builtin_bswap32(w); w = ((w >> 4) & 0x0F0F0F0Fu) | ((w & 0x0F0F0F0Fu) << 4); }
                    uint32_t a = (uint32_t)lt[yy] * 2;
                    for (int c = 0; c < 8; c++, w <<= 4) {
                        uint32_t nib = w >> 28;
                        int32_t xx = sx + c;
                        if (!nib || xx < 0 || xx >= SCREEN_W) continue;
                        uint32_t o = (a + xx) & 0x1FFFF;
                        /* (I11) behind the mist: the SNES adds the fog over the sprites too - a fog dot
                           (32X colours 1-127; sprites are 128+) stays, the sprite shows between them */
                        uint32_t v = *(volatile uint8_t *)(0x24000000 + o);
                        if (v && v < 128) continue;
                        *(volatile uint8_t *)(OVRB + o) = (uint8_t)(L[nib << 4] >> 8);
                    }
                }
            }
        }
        if (nr >= 0) {
            if (nr >= maxr) nr = -1;
            else {
                r[nr][0] = x < 0 ? 0 : x; r[nr][1] = x + size > SCREEN_W ? SCREEN_W : x + size;
                r[nr][2] = y < 0 ? 0 : y; r[nr][3] = y + size > SCREEN_H ? SCREEN_H : y + size;
                nr++;
            }
        }
    }
    return nr;
}
#undef TROWS

/* only canvas tile rows [r0, r1) (ring rows r & 31): a CPU's band in the fused job */
SDRAM_CODE void spr_clear_rows(int bi, int32_t r0, int32_t r1)
{
    uint32_t (*mk)[2] = MARKS_UNCACHED(&spr_marks[bi][0][0]);
    if (r1 - r0 >= 32) { r0 = 0; r1 = 32; }
    for (int32_t r = r0; r < r1; r++) mk[r & 31][0] = mk[r & 31][1] = 0;
}
/* the slave, before drawing sprites: drop its stale cache lines of the sprite list and of the tiles
   the master converted this frame (SH7604 associative purge: a write to 0x40000000 + address) */
#define CCR_PURGE() (*(volatile uint8_t *)0xFFFFFE92 |= 0x10)
SDRAM_CODE void spr_slave_sync(void)
{
#ifdef __sh__
    /* the sprite list (rewritten each frame) and the tiles converted since this CPU last drew
       sprites: drop its whole cache (robust on every SH-2 implementation; ~4k cycles on MiSTer) */
#endif
}
void spr_clear_marks(int bi)
{
    uint32_t (*mk)[2] = MARKS_UNCACHED(&spr_marks[bi][0][0]);
    for (int i = 0; i < 32; i++) mk[i][0] = mk[i][1] = 0;
}
