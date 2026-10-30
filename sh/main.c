/*
 * DKC 32X viewer v2 - SH-2 side.
 *
 * Master SH-2: menu (world maps), level loading (bakes 8bpp tiles into SDRAM), camera, top half
 *              of every frame, HUD, buffer flip, per-scanline sky (H interrupt -> palette 0).
 * Slave SH-2:  bottom half of every frame.
 *
 * Data comes from the pack placed at ROM 0x040000 (built from the user's ROM by
 * tools/build_viewer.py).  Framebuffer: line table + 41x29 tile window, STRIDE bytes per line.
 * Fine scroll via line table + shift register; overlay layers drawn through the framebuffer
 * "overwrite" region, where zero bytes are skipped by the hardware (free transparency).
 */
#include <stdint.h>
#include "music.h"
#include "musplay.h"
#include "game.h"
#include "sprites.h"
#ifdef PERFREC
/* (test builds) PERFREC: level / 60 FPS mode / frame skip come from perf_cfg (patched in the ROM file by
   devkit/perf/perf.py), measurements go to perf_rec (read from an ares SDRAM dump) */
const volatile uint32_t perf_cfg[4] __attribute__((used)) = { 0x50434647, 0, 0, 0 };   /* magic, level, 60 FPS mode, flags (1: no frame skip) */
/* [0] 'DONE' when the window is over, [1] pictures, [2] vblanks, [3] game frames, [4..11] pictures that
   took 1..8+ vblanks, [12] longest picture (vblanks), [13] pictures in 60 FPS mode, [14] slave idle FRT
   ticks, [15] master logic FRT ticks, [16] FRAME_TICKS, [17] first game frame, [18] window frames */
volatile uint32_t perf_rec[64] __attribute__((used, aligned(16)));
uint32_t perf_lost[4];
uint32_t perf_padb, perf_padx;           /* (B presses the logic frames got / the 68000 sent: PADTEST) */
volatile uint32_t perf_sidle;            /* (the slave's wait-for-a-job time, FRT ticks) */
uint32_t perf_acc[24];                    /* (master FRT ticks: 0 logic, 1 60 FPS help, 2 wait for the slave, 3 present, 4 setup+jobs) */
#define PERF_ADD(i, t) (perf_acc[i] += (t))
static uint16_t perf_mt; static uint32_t perf_mv;
uint32_t perf_cnt[16];
static int perf_seen; static uint32_t perf_pv;
uint32_t perf_ln[16];                     /* (60 FPS mode lines: 0 n, 1 sum logic-end, 2 sum slave-done, 3 done before the logic end, 4 done after 222, 5 sum kick, 6 n kick) */
#define PERF_MARK(i) do { extern uint32_t frt_since_(uint16_t, uint32_t); if ((i) >= 0) { perf_acc[(i) < 0 ? 0 : (i)] += frt_since_(perf_mt, perf_mv); perf_cnt[(i) < 0 ? 0 : (i)]++; } do { perf_mv = vcount_(); perf_mt = frt_read_(); } while (perf_mv != vcount_()); } while (0)
#define AUTOLEVEL ((int)perf_cfg[1])
#ifndef GAMEDEMO
#define GAMEDEMO
#endif
#else
#define PERF_ADD(i, t) ((void)0)
#define PERF_MARK(i) ((void)0)
#endif

#define REG8(a)   (*(volatile uint8_t  *)(a))
#define REG16(a)  (*(volatile uint16_t *)(a))
#define COMM(n)   REG16(0x20004020 + (n))

#define SYS_ADAPTER   0x20004000
#define SYS_INTMASK   0x20004001
#define SYS_HCOUNT    0x20004005
#define SYS_VINT_CLR  0x20004016
#define SYS_HINT_CLR  0x20004018
#define VDP_DISPMODE  0x20004100
#define VDP_SHIFT     0x20004102
#define VDP_FBCTL     0x2000410A
#define VDP_VBLK      0x8000
#define CRAM          ((volatile uint16_t *)0x20004200)
#define FB8           ((volatile uint8_t  *)0x24000000)
#define FB16          ((volatile uint16_t *)0x24000000)
#define FB32          ((volatile uint32_t *)0x24000000)
#define OVR8          ((volatile uint8_t  *)0x24020000)

#define FRT_TCR   REG8(0xFFFFFE16)
#define FRT_FRCH  REG8(0xFFFFFE12)
#define FRT_FRCL  REG8(0xFFFFFE13)
#define CCR       REG8(0xFFFFFE92)

#define SCREEN_W  320
#define SCREEN_H  224
#define TCOLS     41
#define TROWS     29
#define STRIDE    336
#define FB_BASE   0x200
#define SPLIT_ROW 14
/* FRT ticks (SH-2 clock / 8) per frame: NTSC 47937, PAL (22.8 MHz, 49.70 Hz) 57348 - m_main */
static uint32_t frame_ticks = 47937;
#define FRAME_TICKS frame_ticks

#define PAD_UP    0x0001
#define PAD_DOWN  0x0002
#define PAD_LEFT  0x0004
#define PAD_RIGHT 0x0008
#define PAD_B     0x0010
#define PAD_C     0x0020
#define PAD_A     0x0040
#define PAD_START 0x0080

/* COMM registers (v2) */
#define C_CAMX   0
#define C_CAMY   2
#define C_FRAME  4
#define C_CMD    6
#define C_PAD    8
#define C_TOKEN  12
#define C_DONE   14
#ifdef LOADSTAGE
#define LDST(n) (COMM(10) = 0xD000 | (n))   /* (test: progress marker, read back via the 68000's dbg_comm) */
#define LDST2(n) (COMM(10) = 0xE000 | (n))
#else
#define LDST(n) ((void)0)
#define LDST2(n) ((void)0)
#endif

#define UNCACHED(p) ((__typeof__(p))((uint32_t)(p) | 0x20000000))
#define SDRAM_CODE __attribute__((section(".sdtext"), aligned(16)))   /* "ax": alignment pads are NOPs, not 0x0000 */
#ifdef TCACHE                            /* (I12) code game builds never run (non-canvas viewer path, the comp
                                            compositor - their fog levels use the performance variant): ROM */
#define SDRAM_VIEWER
#else
#define SDRAM_VIEWER SDRAM_CODE
#endif

#include "../build/gen2/pack.h"

volatile uint32_t dbg_stage;
/* ------------------------------------------------------------ pack structures */
#define PACK ((const uint8_t *)0x02040000)
typedef struct { uint16_t w, h; uint8_t xm, xs, ym, ys; uint16_t ysub, yclamp; uint32_t off_map; } ov_t;
typedef struct {
    uint16_t entrance, world; char name[28];
    uint16_t gw, gh, cx0, cy0;
    int32_t xmin, xmax, ymin, ymax, startx, starty;
    uint32_t off_ts, off_grid, off_pal, off_sky, nov;
    ov_t ov[2];
    uint32_t off_md, md_size, flags;
    uint32_t off_md2, off_fog;       /* comp levels: performance variant (Genesis BG1 + dithered layer) */
    uint32_t off_snap;               /* the level-start snapshot (game builds), 0 = none */
} level_t;                           /* flags bits 24-31: ROM bank shown in window 7 (SSF2) */
#define LV_BANK(lv) ((lv)->flags >> 24)
#define LV_BANK6(lv) (((lv)->flags >> 16) & 0x3F)   /* all-worlds ROM: its world bank (window 6), 0 = none */
typedef struct { uint32_t npool, off_pool, nbake, off_bake, nvch, off_vch, off_vprio; } tileset_t;   /* off_vprio: u16 BG1 priority mask per vchunk (0: none) */
typedef struct { char name[28]; uint32_t nlev; uint16_t idx[8]; uint32_t off_img, off_pal; uint8_t bank, pad[7]; } world_t;
typedef struct { char magic[4]; uint32_t nlev, nworld, off_levels, off_worlds, off_font, off_music, off_rooms, off_maps,
                 off_screens, off_tfont, off_boot, off_fe; } pack_hdr_t;   /* off_boot: the power-on snapshot (complete game) */
/* cabins: Cranky, Funky, Candy pictures; txh = the dialogue layer's h offset */
typedef struct { uint32_t off_img, off_pal; uint8_t bank; int8_t txh; uint8_t pad[2]; } cabin_t;
/* the game's map screens by index ($02 on a map, CODE_80E1F8): 0 the world map, 1 / 2 Kongo Jungle ... */
typedef struct { uint32_t off_img, off_pal; uint8_t bank, pad[3]; } mapscr_t;

#define PK(T, off) ((const T *)(PACK + (off)))

/* ------------------------------------------------------------ SDRAM working set */
#ifdef SLOTHACK                          /* (experiment: memory for SDRAM code, BG garbage) */
#define MAX_SLOTS  540
#define MAX_VCH    64
#else
#define MAX_SLOTS  (PACK_MAX_SLOTS + 2)   /* + the zero slot */
#define MAX_VCH    (PACK_MAX_VCH + 2)
#endif
#define MAX_BAKE   (PACK_MAX_BAKE + 8)
#ifdef TCACHE
/* game builds: BG tiles are baked on demand into small per-CPU caches (direct mapped, by bake index)
   and the vch table is read from the pack: the SDRAM goes to the recompiled game code instead.
   The master's cache (+ tags) is also the game's load-only WRAM scratch (mkgame.py). */
#define TC_MN 436                         /* (index = bake & 511, 448..511 folded onto 320..383: the
                                             SDRAM went to the game's WRAM pages it uses in levels) */
#define TC_MMASK 511
#define TC_SN 256
uint32_t tiles_ram[(TC_MN * 64 + TC_MN * 2) / 4] __attribute__((aligned(16)));
#define tc_mtag ((uint16_t *)(tiles_ram + TC_MN * 16))
static uint32_t tc_sdata[TC_SN * 16] __attribute__((aligned(16)));
static uint16_t tc_stag[TC_SN];
#else
uint32_t tiles_ram[MAX_SLOTS * 16] __attribute__((aligned(16)));   /* (also the game's load-only WRAM scratch) */
static uint16_t vch_ram[MAX_VCH * 16];
#endif
/* slotmap is only used while loading a level; the music echo buffer shares it (music is silenced
   while loading and restarted afterwards, which clears the echo) */
#ifdef TCACHE
/* (game builds bake on demand into the tile caches: no slot map - the union is the echo buffer alone) */
static union { uint16_t map[1]; int8_t echo[2 * ((4 * 512 * MUS_RATE + 31999) / 32000 + 8)]; } slot_echo;
#else
static union { uint16_t map[MAX_BAKE]; int8_t echo[2 * ((4 * 512 * MUS_RATE + 31999) / 32000 + 8)]; } slot_echo;
#endif
#define slotmap slot_echo.map
/* overlay maps (u16 slot | vflip); comp levels convert them in place to comp_map (u32 entries) */
#ifdef MAPMEM_SMALL
static uint32_t map_mem[2][64 * 16];   /* experiment: JH only (no overlays / rain) */
#else
static uint32_t map_mem[2][64 * 64];
#endif
#define ovmap_ram ((uint16_t (*)[64 * 64])map_mem)
static uint16_t sky_ram[SCREEN_H];
/* (I17) the next picture's sky, written while the render prepares that picture (JH sunset table, rain /
   snow fades: those pictures always swap synchronously, wx_dirty) - present() copies it to sky_ram
   after the swap.  Written straight into sky_ram, the lines of the picture still on screen below the
   moment of the write showed the new sky (a torn gradient for one frame). */
static uint16_t sky_next[SCREEN_H];
static int sky_next_on;
/* 1 << n without libgcc's variable shift (the SH-2 has none) */
static const uint32_t bit32[32] = {
    1u << 0, 1u << 1, 1u << 2, 1u << 3, 1u << 4, 1u << 5, 1u << 6, 1u << 7, 1u << 8, 1u << 9, 1u << 10,
    1u << 11, 1u << 12, 1u << 13, 1u << 14, 1u << 15, 1u << 16, 1u << 17, 1u << 18, 1u << 19, 1u << 20,
    1u << 21, 1u << 22, 1u << 23, 1u << 24, 1u << 25, 1u << 26, 1u << 27, 1u << 28, 1u << 29, 1u << 30, 1u << 31 };
static inline uint32_t shr8(uint32_t v, uint32_t n) { if (n & 4) v >>= 4; if (n & 2) v >>= 2; if (n & 1) v >>= 1; return v; }
static uint8_t vused[(MAX_VCH + 7) / 8];
#ifdef TCACHE
static uint8_t rowmask[MAX_BAKE];    /* (by bake index) bit r = tile row r has opaque pixels */
static uint8_t rowfull[MAX_BAKE];    /* bit r = tile row r is fully opaque */
#else
static uint8_t rowmask[MAX_SLOTS];   /* bit r = tile row r has opaque pixels */
static uint8_t rowfull[MAX_SLOTS];   /* bit r = tile row r is fully opaque */
#endif
/* comp mode (true transparency): whole scene on the 32X, composited per line in SDRAM */
static const uint8_t *bake_remap;    /* CGRAM index -> quantized scene colour (comp levels) */
static uint32_t bake_add;            /* added to non-zero pixels (comp performance mode: BG1 unlit bank) */
#ifdef COMPFID
static int comp_perf = 0;
#else
static int comp_perf = 1;            /* comp levels with a performance variant: 1 = use it (B+C toggles) */
#endif
static uint32_t comp_U;              /* first palette entry of the unlit bank */
static int comp;


typedef struct {
    int32_t camx, camy;              /* camy includes the SNES +1 line */
    const uint16_t *grid;
    int32_t gw, gh, ox0, oy0;        /* grid origin in level pixels */
    int32_t nov;
    int32_t ovx[2], ovy[2];          /* overlay scroll for this frame */
    int32_t epoch;
    int32_t slave_ticks, slave_lines;
    int32_t menu;
    int32_t anim_cur[2];             /* first slot of the current animation frame (anim overlays) */
    int32_t wob_phase;               /* heavy snow: per-line VOFS wave phase */
    int32_t ripple_phase;
    int32_t rip;                     /* game ripple: sprites are drawn up to 1 line lower (restore) */
    int32_t split;                   /* comp levels: first screen line of the slave's band */
    int32_t split_row;               /* other levels: first tile row of the slave's band */
    int32_t cv;                      /* incremental canvas mode (see render_rect) */
    /* canvas mode job (see cv_band): window N, still-valid part I, last HUD rect H (tile rects
       x0, x1, y0, y1), full redraw flag, buffer index, the slave's tile rows */
    int32_t cvN[4], cvI[4], cvH[4];
    int32_t cvfull, cvbi, cvrows[2];
    int32_t fogn;                    /* canvas mode 3: tiles per dither phase (phase 1 = slot + fogn) */
    int32_t ovcap;                   /* canvas: the level has overlay passes (restore under them) */
    int32_t glow;                    /* overlay 0/1 = glimmer dither phases (draw only overlay cvbi) */
    int32_t bm_lvl, bm_on, bm_ax, bm_ay;   /* Torchlight: Squawks' beam (apex on screen), see beam_row */
    int32_t bm_game, bm_kind;        /* ... in the game: edges per line (BM_LN), dark pixels via bm_dmap */
    /* rain phase (job 1): both CPUs restore + draw the rain on their half of the screen */
    int32_t job, rbuf, rH, rV, rcut, rdraw, rn, rx0par;
    const uint8_t *rpix;
    uint32_t rcntB[2];               /* the slave's save-list lengths per buffer */
    int32_t spr;                     /* game sprites drawn in the canvas: restore under spr_marks */
    int32_t sgx, sgy, sbi, ssplit;   /* sprite job (2): canvas origin, buffer, first line of the slave's band */
    int32_t pw[4], pbi;              /* early restore job (3): the buffer's valid window, buffer */
    int32_t ph[4];                   /* job 3: the buffer's last HUD rect (restored early) */
    int32_t lt_do, ltx, lty;         /* job 2: the slave writes the canvas line table first */
    int32_t spr_sticks;              /* the slave's time for its sprite band (job 2) */
    int32_t j3_ticks;                /* the slave's early job (3) time */
    int32_t ov_rows;                 /* job 3: tile rows restored under the old overlay pass (adaptive) */
    int32_t pdraw;                   /* job 3: also draw the strips of the predicted window (cvN/cvI) */
    int32_t frows[2];                /* job 5 (fused band): the slave's canvas tile rows (the top band) */
    int32_t pmline;                  /* (I17) 60 FPS mode: first screen line of the master's BG rows */
    int32_t hud_on;                  /* job 5: draw the HUD line hud[] (in the slave's top band) */
    int32_t phelp;                   /* job 5, 60 FPS mode: sprite bands shared with the master (pb_*) */
    char hud[20];
    const uint16_t *vch;             /* the level's vch table (vch_ram, or the pack in TCACHE builds) */
    const void *ts;                  /* (tileset_t) */
    uint32_t *tcd;                   /* TCACHE: this CPU's tile cache: data, tags, index mask */
    uint16_t *tct;
    uint32_t tcm, tcn;               /* index mask, entries (index >= tcn: - 128) */
} shared_t;

/* BG tile (bake index bk, 64 bytes 8bpp) for this CPU: tiles_ram slot, or the TCACHE cache */
#ifdef TCACHE
/* (H17) a composite bake entry (flag 2: top bake << 19 | its vflip << 18 | under bake << 5 | its
   vflip << 4) - one tile in front of another (Mine Cart Carnage: BG2's track bed, streamed by the game from the level layout + 4B00, under
   BG1's rails) - both baked into one 8bpp tile, byte order as tc_fill's */
__attribute__((noinline)) static void tc_composite(const tileset_t *ts, uint8_t *d, uint32_t c, int dark)
{
    for (int part = 1; part >= 0; part--) {
        uint32_t be = PK(uint32_t, ts->off_bake)[part ? (c >> 5) & 0x1FFF : c >> 19];
        uint32_t pidx = be >> 16, pal = ((be >> 8) & 0xFF) * 4, hf = be & 1, vf = part ? (c >> 4) & 1 : (c >> 18) & 1;
        const uint8_t *src = PACK + ts->off_pool + pidx * 32;
        for (int r = 0; r < 8; r++) {
            const uint8_t *sr = src + (vf ? 7 - r : r) * 4;
            for (int x = 0; x < 8; x++) {
                uint32_t sx = hf ? 7 - x : x;
                uint32_t v = (sx & 1) ? sr[sx >> 1] & 15 : sr[sx >> 1] >> 4;
                if (part) d[r * 8 + x] = v ? (uint8_t)(pal + v) : 0;
                else if (v) d[r * 8 + x] = (uint8_t)(pal + v);
            }
        }
    }
    if (dark) for (int i = 0; i < 64; i++) d[i] = ((const uint8_t *)map_mem + 24576)[d[i] & 0x7F];
}
#define BM_DMAP_T ((const uint8_t *)map_mem + 24576)    /* (= bm_dmap below) */
/* cache slot i <- tag (bake key; | 0x8000: Torchlight's lit tile under its own key) */
SDRAM_CODE __attribute__((noinline)) static void tc_fill(const shared_t *p, uint32_t i, uint32_t tag)
{
    uint32_t *d = p->tcd + i * 16, bk = tag & 0x7FFF;
    int dark = p->bm_game && !(tag & 0x8000);
    p->tct[i] = (uint16_t)tag;
    const tileset_t *ts = (const tileset_t *)p->ts;
    uint32_t be = PK(uint32_t, ts->off_bake)[bk];
    if (be & 2) {
        tc_composite(ts, (uint8_t *)d, be, dark);
        return;
    }
    uint32_t pidx = be >> 16, pal = ((be >> 8) & 0xFF) * 4, hf = be & 1;
    const uint32_t *src = (const uint32_t *)(PACK + ts->off_pool + pidx * 32);
    uint8_t t[16];                                   /* nibble -> colour index (0 stays 0) */
    t[0] = 0;
    /* (I12) Torchlight: the cache holds the dark tiles (and the lit ones under other keys: tile_ptr_lit) */
    const uint8_t *dm = BM_DMAP_T;
#pragma GCC unroll 0
    for (int n = 1; n < 16; n++) t[n] = dark ? dm[(pal + n) & 0x7F] : (uint8_t)(pal + n);
    for (int r = 0; r < 8; r++) {
        uint32_t w = src[r];
        uint32_t a0 = t[w >> 28], a1 = t[(w >> 24) & 15], a2 = t[(w >> 20) & 15], a3 = t[(w >> 16) & 15];
        uint32_t a4 = t[(w >> 12) & 15], a5 = t[(w >> 8) & 15], a6 = t[(w >> 4) & 15], a7 = t[w & 15];
        if (hf) {
            d[r * 2] = a7 << 24 | a6 << 16 | a5 << 8 | a4;
            d[r * 2 + 1] = a3 << 24 | a2 << 16 | a1 << 8 | a0;
        } else {
            d[r * 2] = a0 << 24 | a1 << 16 | a2 << 8 | a3;
            d[r * 2 + 1] = a4 << 24 | a5 << 16 | a6 << 8 | a7;
        }
    }
}
static inline const uint32_t *tile_ptr(const shared_t *p, uint32_t bk)
{
    uint32_t i = bk & p->tcm;
    if (i >= p->tcn) i -= 128;
    uint32_t *d = p->tcd + i * 16;
    if (p->tct[i] != bk) tc_fill(p, i, bk);
    return d;
}
/* (I12, Torchlight) the lit tile: tag bk | 0x8000, another slot */
__attribute__((noinline)) static const uint32_t *tile_ptr_lit(const shared_t *p, uint32_t bk)
{
    uint32_t i = (bk + 0x55) & p->tcm;
    if (i >= p->tcn) i -= 128;
    uint32_t *d = p->tcd + i * 16;
    if (p->tct[i] != (bk | 0x8000)) tc_fill(p, i, bk | 0x8000);
    return d;
}
#define VCH(p) ((p)->vch)
#else
static inline const uint32_t *tile_ptr(const shared_t *p, uint32_t slot) { (void)p; return tiles_ram + (slot << 4); }
#define VCH(p) vch_ram
#endif
static shared_t shared_state;
/* 60 FPS mode: the sprites of the slave's picture in 16-line bands (on canvas tile rows), taken from
   the top by the slave and from the bottom by the master once its logic is done.  A band both take at
   once is drawn twice (same pixels, same marks): harmless.  pb_go = the job's token once the slave's
   BG work is done (sprites go on top of it). */
static volatile int32_t pb_next, pb_low, pb_go;
/* (I17) 60 FPS mode: the master draws the BG of the bottom rows (screen lines pmline..) right after the
   kick, before its logic; pipe_mbg = that job's token once done (the slave's sprite bands over those
   rows wait for it).  pipe_spl adapts: slave late -> the master takes more rows, master late -> fewer. */
static volatile int32_t pipe_mbg = -1;
static int32_t pipe_spl = SCREEN_H, pipe_vk;
#define PB_MAX 16
static int32_t pb_edge(int32_t gy, int32_t b)
{
    int32_t base = (8 - (gy & 7)) & 7;          /* first screen line on a canvas tile row */
    int32_t y = b == 0 ? 0 : base + (b - 1) * 16;
    return y > SCREEN_H ? SCREEN_H : y;
}
static int32_t pb_count(int32_t gy)
{
    int32_t n = 1;
    while (pb_edge(gy, n) < SCREEN_H) n++;
    return n;                                    /* bands 0 .. n-1: [edge(b), edge(b + 1)) */
}
static ov_t ovinfo[2];
/* animated overlays (heavy snow, CODE_8089A0 weather level 5) */
static int32_t anim_slot0[2], anim_n[2], anim_frame;
static uint8_t snow_sway[64];
static int8_t snow_wob[32];
/* heavy snow flake lists (pack, ROM): per animation frame u16 row index[257] + (x, colour) pairs */
static const uint8_t *flake_frame[8];

static volatile int32_t hline;
/* underwater BG1 ripple (SNES HDMA ch5): 64-line cycle of bands shifted (+1,+1) - 22 on, 11 off,
   10 on, 21 off - sliding down one line per frame.  Vertical/even part via the line table, the odd
   pixel via the shift register, rewritten per line by the H interrupt from sft_tab. */
static uint8_t sft_tab[2][SCREEN_H];
static const uint8_t *volatile sft_show;      /* table of the frame on screen (0 = no ripple) */
/* (I17) a picture swap requested during the display (present, FLIP_ASYNC): the V interrupt that
   follows is the swap's vblank - it sets the picture's shift register value */
static volatile int32_t af_vint;
static volatile uint16_t af_shift;
static const uint8_t *volatile af_sft;        /* (I19) ripple: the new picture's shift table, set at the swap */
static int ripple, ripple_phase;
static uint8_t hint_mask = 0x0C;
static uint8_t ripple_pat[64];
static int32_t comp_wave[SCREEN_H / 4][2];   /* comp: light {H, V} per 4-line band */
static volatile uint32_t vcount;
/* 60 Hz game ticks: = vcount on NTSC; a PAL console (50 Hz) adds an extra tick every 5th vblank, and
   the game loop runs as many logic frames as ticks passed - the game keeps its NTSC speed (6 logic
   frames per 5 pictures).  PALSLOW builds run 1 tick per vblank (50 Hz, 5/6 speed, like PAL SNES ports
   that were not adjusted). */
static volatile uint32_t gtick;
static int pal, pal_ph;

static inline uint16_t frt_read(void)
{
    uint8_t hi = FRT_FRCH;
    return ((uint16_t)hi << 8) | FRT_FRCL;
}

/* ------------------------------------------------------------ sky interrupts (v1.1) */
#define FRT_LINE_RECIP 1434
static volatile uint16_t t_vint;
static volatile int32_t line_k = 0x7FFF, calib_frames, first_hint;
static int frt_ok;

/* hang watchdog: the master's loop clears hang_vbl every iteration; ~5 s without one (in a level)
   and the V interrupt shows where both CPUs were (hang_show, never returns) */
static volatile uint32_t hang_vbl, hang_armed, s_job_now;
volatile uint32_t m_irq_pc, s_irq_pc;   /* interrupted PCs at the last V interrupt (start.s stubs) */
static void hang_show(void);

/* (I17) the pad, one sample per vblank in order: the 68000 writes COMM(C_PAD) = sample number << 8 |
   pad once per vblank.  The V interrupt and pad_take store every new sample (pad_ring, by number);
   each logic frame takes the next one, so frames run back to back under frame skip see the vblanks'
   samples in order like the SNES (before, both read the register: a one-vblank press could fall
   between two pictures and a jump was missed).  More samples than logic frames left in a picture
   (the game fell behind): the oldest are passed over, but their new button presses go into the next
   sample taken.  The last logic frame of a picture always gets the newest sample (no added lag). */
static volatile uint32_t m68_pct;          /* (I21c) the 68000's load in %, sent bit by bit in C_PAD bits 14-15 */
static volatile uint8_t pad_ring[16];
static volatile uint32_t pad_new;         /* number of the newest sample in pad_ring */
static uint32_t pad_cons, pad_tv;         /* main loop: number of the last sample taken; vcount then */
static volatile uint32_t pad_sv;          /* vcount at the last new sample */
static inline void pad_store(void)       /* (V interrupt, or with it masked) */
{
    uint16_t p = COMM(C_PAD);
    uint32_t s = (p >> 8) & 0x3F, d = (s - pad_new) & 0x3F;   /* (I21c: 6-bit sample number) */
    if (!d) return;
    {   /* (I21c) bits 15 / 14: the 68000's load, 7 bits MSB first, one a vblank (14 = the first bit) */
        static uint32_t acc, n;
        if (d != 1) n = 0;
        else if (p & 0x4000) { acc = p >> 15; n = 1; }
        else if (n) { acc = acc << 1 | p >> 15; if (++n == 7) { m68_pct = acc; n = 0; } }
    }
    if (d > 16) d = 16;
    for (uint32_t k = 0; k < d; k++)      /* (samples the V interrupt missed: the newest instead) */
        pad_ring[(s - k) & 15] = (uint8_t)p;
#ifdef PERFREC
    {   extern uint32_t perf_padx; for (uint32_t k = 0; k < d; k++) if (((s - k) & 31) == 0) perf_padx++; }
#endif
    pad_new = s;
    pad_sv = vcount;
}
#define PAD_NOW() ((uint16_t)(COMM(C_PAD) & 0xFF))

__attribute__((interrupt_handler)) SDRAM_CODE void master_vint(void)
{
    REG16(SYS_VINT_CLR) = 0;
#ifndef HANG_VBL
#define HANG_VBL 300
#endif
    if (hang_armed && ++hang_vbl == HANG_VBL) hang_show();
    t_vint = frt_read();
    first_hint = 1;
    CRAM[0] = sky_ram[0];
    {
        const uint8_t *st = sft_show;
        if (st)
            REG16(VDP_SHIFT) = st[0];
    }
    if (af_vint) {
        const uint8_t *ns = af_sft;
        if (ns) {                           /* (I19) ripple pictures swap asynchronously too */
            sft_show = ns;
            REG16(VDP_SHIFT) = ns[0];
            af_sft = 0;
        } else
            REG16(VDP_SHIFT) = af_shift;
        af_vint = 0;
    }
    hline = 1;
    pad_store();
    vcount++;
    gtick++;
#ifndef PALSLOW
    if (pal && ++pal_ph == 5) {
        pal_ph = 0;
        gtick++;
    }
#endif
    (void)REG16(SYS_VINT_CLR);
}

/* the slave can own the per-line sky colour (and ripple shift): the master then takes no H
   interrupts (each one also evicts game code from its instruction cache).  Lines are counted from
   the slave's V interrupt; the music DMA interrupt (level 9) does not block them. */
static volatile int32_t s_hline;
static volatile uint32_t slave_hmask;     /* the slave's 32X interrupt enables (bit 0 kept): 0 / 0x0C */
__attribute__((interrupt_handler)) SDRAM_CODE void slave_vint(void)
{
    REG16(SYS_VINT_CLR) = 0;
    s_hline = 1;
    CRAM[0] = *UNCACHED(&sky_ram[0]);
    {
        const uint8_t *st = *UNCACHED(&sft_show);
        if (st) REG16(VDP_SHIFT) = *UNCACHED(st);
    }
    (void)REG16(SYS_VINT_CLR);
}
__attribute__((interrupt_handler)) SDRAM_CODE void slave_hint(void)
{
    REG16(SYS_HINT_CLR) = 0;
    int32_t l = s_hline;
    s_hline = l + 1;
    if (l > 0 && l < SCREEN_H) {
        CRAM[0] = *UNCACHED(&sky_ram[l]);
        const uint8_t *st = *UNCACHED(&sft_show);
        if (st) REG16(VDP_SHIFT) = *UNCACHED(st + l);
    }
    (void)REG16(SYS_HINT_CLR);
}

static inline int32_t frt_line_number(void)
{
    uint32_t dt = (uint16_t)(frt_read() - t_vint);
    return (int32_t)((dt * FRT_LINE_RECIP + (1u << 17)) >> 18);
}

__attribute__((interrupt_handler)) SDRAM_CODE void master_hint(void)
{
    int32_t l;
    REG16(SYS_HINT_CLR) = 0;
    if (frt_ok) {
        int32_t n = frt_line_number();
        if (first_hint) {
            first_hint = 0;
            if (calib_frames < 32) {
                calib_frames++;
                if (n < line_k) line_k = n;
            }
        }
        l = n - line_k + 1;
    } else {
        l = hline;
    }
    hline = l + 1;
    if (l > 0 && l < SCREEN_H) {
        CRAM[0] = sky_ram[l];
        const uint8_t *st = sft_show;
        if (st)
            REG16(VDP_SHIFT) = st[l];
    }
    (void)REG16(SYS_HINT_CLR);
}

static int32_t current_line(void)
{
    if (frt_ok && calib_frames)
        return frt_line_number() - line_k;
    return hline - 1;
}

/* ------------------------------------------------------------ scroll formulas (same as builder) */
static int32_t scroll_val(int32_t cam, int mode, int shift, int sub, int clamp)
{
    uint32_t v = (uint16_t)(cam - sub);
    if (clamp != 0xFFFF && v > (uint32_t)clamp) v = clamp;
    switch (mode) {
    case 0: return v >> shift;
    case 1: return v + (v >> 1);
    case 2: return v << 1;
    case 4: return (v >> shift) & 0xFF;
    default: return shift == 0xFF ? -1 : shift;
    }
}

/* ------------------------------------------------------------ renderer */
/* one row of BG1 tiles (grid tile row ty, columns x0..x1) at d, line stride L longs; zero outside
   the grid */
SDRAM_CODE static void draw_tile_row(const shared_t *p, volatile uint32_t *d, int32_t ty, int32_t x0, int32_t x1,
                                     uint32_t L)
{
    const uint32_t *tiles = tiles_ram;
    int32_t cy = ty >> 2;
    const uint16_t *grow = p->grid + cy * p->gw;
    const uint16_t *vrow = VCH(p) + (ty & 3) * 4;
    int row_ok = ty >= 0 && cy < p->gh;
    for (int32_t tx = x0; tx < x1; tx++, d += 2) {
        int32_t cx = tx >> 2;
        volatile uint32_t *q = d;
        if (!row_ok || tx < 0 || cx >= p->gw) {
            for (int k = 0; k < 8; k++, q += L) { q[0] = 0; q[1] = 0; }
            continue;
        }
        uint32_t e = vrow[grow[cx] * 16 + (tx & 3)];
        const uint32_t *s = tile_ptr(p, e & 0x7FFF);
        if (e & 0x8000) {
            s += 14;
            for (int k = 0; k < 8; k++, q += L, s -= 2) { q[0] = s[0]; q[1] = s[1]; }
        } else {
            for (int k = 0; k < 8; k++, q += L, s += 2) { q[0] = s[0]; q[1] = s[1]; }
        }
    }
}

SDRAM_VIEWER static void render_rows(const shared_t *p, int r0, int r1)
{
    int32_t lx = p->camx - p->ox0, ly = p->camy - p->oy0;
    int32_t tx0 = lx >> 3, ty0 = ly >> 3;
    volatile uint32_t *drow = FB32 + (FB_BASE + r0 * 8 * STRIDE) / 4;
    for (int r = r0; r < r1; r++, drow += 8 * STRIDE / 4)
        draw_tile_row(p, drow, ty0 + r, tx0, tx0 + TCOLS, STRIDE / 4);
}

/* ---- incremental canvas (normal levels without overlay layers) ----
   The 32X has no tilemap, so BG1 used to be redrawn in full every frame (~1260 tiles).  Instead each
   framebuffer keeps a canvas of the level around the camera: grid tile (tx, ty) lives at canvas line
   (8 ty) mod CV_H, byte offset 8 tx + CV_XOFF.  Vertical scrolling wraps through the line table;
   horizontal scrolling just moves every line's start address (a line's span never reaches the next
   line's visible part: CV_P >= 43 tiles, and levels are < 47000 pixels wide).  Each frame only the
   tiles that entered the window since this buffer was last drawn (two frames ago) are drawn, plus
   the ones under the HUD. */
#define CV_P    352
#define CV_H    256
#define CV_XOFF 64
#define CV_TW   42
#define CV_TH   30
typedef struct { int32_t x0, x1, y0, y1; } trect_t;
typedef struct { int32_t valid; trect_t w, hud; } cvbuf_t;
static cvbuf_t cvb[2];
/* canvas origin (grid pixels at screen 0,0): BG1, or for mode 2 the transparency layer (BG3 x 1.5) */
/* (Poison Pond's fog, mode 2: 1.5 x the camera on both axes - in front of the playfield, for depth) */
#define CV_GX(p, cx) ((p)->cv == 2 ? (cx) + ((cx) >> 1) : (cx) - (p)->ox0)
#define CV_GY(p, cy) ((p)->cv == 2 ? (cy) + ((cy) >> 1) : (cy) - (p)->oy0)
/* per buffer: BG1 tiles under its last overlay pass; the slave's marks for the current frame.  In the
   upper half of map_mem, which only comp levels use (and they have no canvas) */
#define cv_ovd   ((uint32_t (*)[32][2])((uint8_t *)map_mem + 16384))
#define cv_mmask ((uint32_t (*)[2])((uint8_t *)map_mem + 16384 + 512))
#define cv_smask ((uint32_t (*)[2])((uint8_t *)map_mem + 16384 + 768))
#ifdef CVCHECK
static uint32_t cv_errors;
#endif
#ifdef CVCHECK2
static uint32_t cv2_last, cv2_frames;
#endif

/* canvas mode 2 (comp levels, performance variant): the transparency layer, a 32x32 tile map
   (ovmap_ram[0]: slot | vflip << 15) wrapping every 256 pixels, drawn as a checkerboard - the
   other half of the pixels are 0 (the Genesis scene shows through).  Each framebuffer uses the
   opposite phase, so on screen the two alternate every frame. */
static const uint32_t *volatile fog_tiles = tiles_ram;   /* perf variant: tiles_ram, or the pack (ROM) when they do not fit */
SDRAM_CODE static void draw_fog_row(const shared_t *p, volatile uint32_t *d, int32_t ty, int32_t x0, int32_t x1)
{
    const uint32_t *tiles = *UNCACHED(&fog_tiles);
    const uint16_t *mrow = ovmap_ram[0] + (ty & 31) * 32;
    uint32_t ph = p->cvbi & 1;                            /* pixel shown when (x + y + buffer) is even */
    uint32_t mk0 = ph ? 0x00FF00FF : 0xFF00FF00, mk1 = ~mk0;
    if (p->cv == 3) {                                     /* pattern baked per phase */
        uint32_t add = p->cvbi ? p->fogn : 0;
        for (int32_t tx = x0; tx < x1; tx++, d += 2) {
            const uint32_t *s = tiles + ((mrow[tx & 31] + add) << 4);
            volatile uint32_t *q = d;
            for (int k = 0; k < 8; k++, q += CV_P / 4, s += 2) { q[0] = s[0]; q[1] = s[1]; }
        }
        return;
    }
    for (int32_t tx = x0; tx < x1; tx++, d += 2) {
        uint32_t e = mrow[tx & 31];
        const uint32_t *s = tiles + ((e & 0x7FFF) << 4);
        volatile uint32_t *q = d;
        int32_t st = 2;
        if (e & 0x8000) { s += 14; st = -2; }
        for (int k = 0; k < 8; k += 2, q += CV_P / 2, s += 2 * st) {
            q[0] = s[0] & mk0;           q[1] = s[1] & mk0;
            q[CV_P / 4] = s[st] & mk1;   q[CV_P / 4 + 1] = s[st + 1] & mk1;
        }
    }
}

/* hanging lamps: the shade and the light cone baked into the canvas as its tiles are drawn.  One
   image per level ([224][176] colour indices, origin at level (lamp x - 91, 481 - lamp y)); cone
   pixels land where BG1 is transparent, every other pixel in level space (the other half in the
   other framebuffer), shade pixels always */
static uint32_t lamp_n;
static const uint16_t *lamp_pos;
static const uint8_t *lamp_img;
static int32_t lamp_h;                 /* image rows (the shade) */
static volatile int32_t lamp_noshade;   /* game levels: the game draws the lamp (sprite CeilingLight): no baked shade */
__attribute__((noinline)) static void lamp_row(const shared_t *p, volatile uint32_t *d, int32_t ty, int32_t x0, int32_t x1)
{
    int32_t cy = ty >> 2;
    const int32_t noshade = *UNCACHED(&lamp_noshade);
    if (ty < 0 || cy >= p->gh) return;
    const uint16_t *grow = p->grid + cy * p->gw;
    const uint16_t *vrow = VCH(p) + (ty & 3) * 4;
    for (uint32_t i = 0; i < lamp_n; i++) {
        int32_t gy0 = 481 - (int32_t)lamp_pos[2 * i + 1] - p->oy0;
        int32_t iy0 = ty * 8 - gy0;
        if (iy0 <= -8 || iy0 >= lamp_h) continue;
        int32_t gx0 = (int32_t)lamp_pos[2 * i] - 91 - p->ox0;
        int32_t ta = gx0 >> 3, tb = (gx0 + 176 + 7) >> 3;
        if (ta < x0) ta = x0;
        if (tb > x1) tb = x1;
        if (tb > p->gw * 4) tb = p->gw * 4;
        for (int32_t tx = ta < 0 ? 0 : ta; tx < tb; tx++) {
            uint32_t e = vrow[grow[tx >> 2] * 16 + (tx & 3)];
            const uint8_t *s = (const uint8_t *)tile_ptr(p, e & 0x7FFF);
            int32_t sst = 8;
            if (e & 0x8000) { s += 56; sst = -8; }
            volatile uint32_t *q = d + (tx - x0) * 2;
            int32_t ix0 = tx * 8 - gx0;
            for (int32_t r = 0; r < 8; r++, s += sst, q += CV_P / 4) {
                int32_t iy = iy0 + r;
                if (iy < 0 || iy >= lamp_h) continue;
                const uint8_t *ir = lamp_img + iy * 176;
                uint8_t o[8];
                int ch = 0;
                uint32_t ph = (ty * 8 + r + tx * 8 + p->cvbi) & 1;
                for (int32_t c = 0; c < 8; c++) {
                    int32_t ix = ix0 + c;
                    uint8_t b = s[c], v = (ix >= 0 && ix < 176) ? ir[ix] : 0;
                    o[c] = b;
                    if (!v) continue;
                    if ((v >= 128 && !(v & 15) && !noshade) || (v < 128 && !b && ((c ^ ph) & 1) == 0)) { o[c] = v; ch = 1; }
                }
                if (ch) {
                    q[0] = (uint32_t)o[0] << 24 | o[1] << 16 | o[2] << 8 | o[3];
                    q[1] = (uint32_t)o[4] << 24 | o[5] << 16 | o[6] << 8 | o[7];
                }
            }
        }
    }
}

/* Torchlight Trouble: Squawks' flashlight.  The scene is dark (32X CRAM 1-127) except inside a wedge
   from the apex (bm_ax, bm_ay) on screen: right of L(y) = ax + max((ay - y) * 1078, (y - ay) * 64) / 256
   (the SNES window edges: top -0.2375 px/px, lower-left +0.25).  Lit BG1 pixels use CRAM 128 + c
   (the lit palette): colour | 0x80 on the non-zero bytes.  The canvas keeps per buffer and tile row
   the last dark/lit tile bounds (map_mem + 20480), so only the tiles the edges sweep are redrawn. */
#define BM_SOFT 4                 /* soft edge width (checkerboard) */
#define BM_ST ((volatile int16_t *)(((uint32_t)map_mem + 20480) | 0x20000000))   /* [2][32][3] */
/* game levels (I10): the game's own window table (game_beam_lines: the apex follows Squawks, the top
   edge's slope his pose), copied here while the slave is idle; CRAM 1-127 = the lit colours (the
   sprite palettes keep 128-252), dark pixels = bm_dmap[c]: ~13 spare CRAM entries (BG / sprite colour
   0 slots) with the scene's few dark colours (c - 15 leaves black and some reds) */
#define BM_LN ((volatile int16_t *)(((uint32_t)map_mem + 21376) | 0x20000000))    /* [224][2] first, last lit x */
/* (I12: in map_mem's upper half - the lower 16 KB is the sprites' chunky tile copy now:
   Torchlight drew its sprites from the planar tiles, half the master's time) */
#define bm_dmap    ((uint8_t *)map_mem + 24576)                /* [128] */
#define bm_lines   ((int16_t *)((uint8_t *)map_mem + 24704))   /* [224][2] master: the next canvas job's edges */
#define bm_shown   ((int16_t *)((uint8_t *)map_mem + 30912))   /* (I14) [224][2] the edges drawn (-> 31808) */
#define bm_slotcol ((uint16_t *)((uint8_t *)map_mem + 25600))  /* [BM_NS] */
#define BM_TMP     ((int32_t *)((uint8_t *)map_mem + 25664))   /* bm_game_setup: 4 x [128] + [128] bytes (-> 27840) */
static inline int32_t bm_L(const shared_t *p, int32_t y)
{
    int32_t d = y - p->bm_ay, a = (-d * 1078) >> 8, b = (d * 64) >> 8;
    return p->bm_ax + (a > b ? a : b);
}
/* the dark colours of a pixel word (bytes not in m: the lit ones stay) */
static inline uint32_t bm_dark(uint32_t s, uint32_t m)
{
    /* (I12: constant shifts - the SH-2 has no variable shift, the loop called a library shift per
       byte: half the slave's time in Torchlight) */
    if (!s || m == 0x80808080u) return s;
#ifdef BMNODARK
    return s;                             /* (test: the cost of the dark mapping) */
#endif
    const uint8_t *dm = bm_dmap;
    uint32_t b0 = s >> 24, b1 = (s >> 16) & 0xFF, b2 = (s >> 8) & 0xFF, b3 = s & 0xFF;
    if (!(m & 0x80000000u)) b0 = dm[b0 & 0x7F];
    if (!(m & 0x00800000u)) b1 = dm[b1 & 0x7F];
    if (!(m & 0x00008000u)) b2 = dm[b2 & 0x7F];
    if (!(m & 0x00000080u)) b3 = dm[b3 & 0x7F];
    return b0 << 24 | b1 << 16 | b2 << 8 | b3;
}
/* dark word d, lit word l, lit lanes m (0x80 each): the mix */
static inline uint32_t bm_mix(uint32_t d, uint32_t l, uint32_t m)
{
    uint32_t f = (m >> 7) * 0xFF;
    return (l & f) | (d & ~f);
}
/* lit-pixel masks of a tile row (byte lane x = pixel x): pixels x >= a (a 0-8), lanes 0-3 / 4-7 */
static const uint32_t bm_ge0[9] = { 0x80808080u, 0x00808080u, 0x00008080u, 0x00000080u, 0, 0, 0, 0, 0 };
static const uint32_t bm_ge1[9] = { 0x80808080u, 0x80808080u, 0x80808080u, 0x80808080u, 0x80808080u,
                                    0x00808080u, 0x00008080u, 0x00000080u, 0 };
static inline uint32_t bm_lit(uint32_t s, uint32_t m)
{
    uint32_t t = s | (s >> 4);
    t |= t >> 2;
    t |= t >> 1;
    return s | ((t & 0x01010101u) << 7 & m);
}
/* tiles [x0, x1) of grid row ty at d (the canvas address of tile x0), lit right of the edge */
__attribute__((noinline)) static void beam_row(const shared_t *p, volatile uint32_t *d, int32_t ty, int32_t x0, int32_t x1)
{
    int32_t gx = p->camx - p->ox0, ys = ty * 8 - (p->camy - p->oy0);
    int32_t Lr[8], lmin = 0x7FFFFFF;
    for (int r = 0; r < 8; r++) {
        Lr[r] = p->bm_on ? bm_L(p, ys + r) + gx : 0x7FFFFFF;
        if (Lr[r] < lmin) lmin = Lr[r];
    }
    int32_t xd = lmin >> 3;                       /* tiles left of xd are dark */
    if (xd > x1) xd = x1;
    if (xd > x0) {
        draw_tile_row(p, d, ty, x0, xd, CV_P / 4);
        d += (xd - x0) * 2;
        x0 = xd;
    }
    if (x0 >= x1) return;
    int32_t cy = ty >> 2;
    int row_ok = ty >= 0 && cy < p->gh;
    const uint16_t *grow = p->grid + cy * p->gw;
    const uint16_t *vrow = VCH(p) + (ty & 3) * 4;
    for (int32_t tx = x0; tx < x1; tx++, d += 2) {
        volatile uint32_t *q = d;
        int32_t cx = tx >> 2;
        if (!row_ok || tx < 0 || cx >= p->gw) {
            for (int k = 0; k < 8; k++, q += CV_P / 4) { q[0] = 0; q[1] = 0; }
            continue;
        }
        uint32_t e = vrow[grow[cx] * 16 + (tx & 3)];
        const uint32_t *s = tile_ptr(p, e & 0x7FFF);
        int32_t st = 2;
        if (e & 0x8000) { s += 14; st = -2; }
        for (int r = 0; r < 8; r++, q += CV_P / 4, s += st) {
            int32_t c = Lr[r] - tx * 8;           /* first lit pixel of this tile row */
            uint32_t m0, m1;
            if (c <= -BM_SOFT) m0 = m1 = 0x80808080u;
            else if (c >= 8) m0 = m1 = 0;
            else {
                /* soft edge: the first BM_SOFT lit pixels every other one (level space, the other half
                   in the other framebuffer) */
                uint32_t ph = (uint32_t)(ty * 8 + r + p->cvbi) & 1, m[2] = { 0, 0 };
                for (int32_t x = 0; x < 8; x++)
                    if (x >= c + BM_SOFT || (x >= c && !((x + ph) & 1)))
                        m[x >> 2] |= 0x80u << (8 * (3 - (x & 3)));
                m0 = m[0]; m1 = m[1];
            }
            q[0] = bm_lit(s[0], m0);
            q[1] = bm_lit(s[1], m1);
        }
    }
}
/* this buffer's rows [ra, rb): redraw the tiles between the old and new dark / lit bounds */
__attribute__((noinline)) static void beam_pass(const shared_t *p, int32_t ra, int32_t rb)
{
    const int32_t *N = p->cvN;
    int32_t gx = p->camx - p->ox0, gy = p->camy - p->oy0;
    if (ra < N[2]) ra = N[2];
    if (rb > N[3]) rb = N[3];
    for (int32_t ty = ra; ty < rb; ty++) {
        int32_t ys = ty * 8 - gy, lmin = 0x7FFFFFF, lmax = -0x7FFFFFF;
        if (p->bm_on)
            for (int r = 0; r < 8; r += 7) {     /* (L is V-shaped: the ends and the apex) */
                int32_t v = bm_L(p, ys + r) + gx;
                if (v < lmin) lmin = v;
                if (v > lmax) lmax = v;
            }
        if (p->bm_on && ys <= p->bm_ay && p->bm_ay < ys + 8 && p->bm_ax + gx < lmin) lmin = p->bm_ax + gx;
        int32_t D = p->bm_on ? lmin >> 3 : N[1], Lt = p->bm_on ? (lmax + BM_SOFT + 7) >> 3 : N[1];
        if (D < N[0]) D = N[0];
        if (D > N[1]) D = N[1];
        if (Lt < N[0]) Lt = N[0];
        if (Lt > N[1]) Lt = N[1];
        volatile int16_t *e = BM_ST + (p->cvbi * 32 + (ty & 31)) * 3;
        if (!p->cvfull && e[0] == (int16_t)ty) {
            int32_t a = e[1] < D ? e[1] : D, b = e[2] > Lt ? e[2] : Lt;
            if (a < N[0]) a = N[0];
            if (b > N[1]) b = N[1];
            if (a < b) {
                int32_t cl = (ty * 8) & (CV_H - 1);
                beam_row(p, FB32 + (FB_BASE + cl * CV_P + CV_XOFF + a * 8) / 4, ty, a, b);
            }
        }
        e[0] = (int16_t)ty; e[1] = (int16_t)D; e[2] = (int16_t)Lt;
    }
}

/* (I10/I11) the game's beam: per picture line the lit span [L, R] from the game's window table (BM_LN,
   both edges: Squawks faces either way); dark pixels -> bm_dmap.  Tiles [x0, x1) of grid row ty. */
#ifdef BMCOUNT
volatile uint32_t bmc_tiles, bmc_calls;   /* (test) beamg_row work */
#endif
__attribute__((noinline)) static void beamg_row(const shared_t *p, volatile uint32_t *d, int32_t ty, int32_t x0, int32_t x1)
{
#ifdef BMCOUNT
    *UNCACHED(&bmc_tiles) += x1 - x0; *UNCACHED(&bmc_calls) += 1;
#endif
    int32_t gx = p->camx - p->ox0, ys = ty * 8 - (p->camy - p->oy0);
    int32_t Lr[8], Rr[8];
    for (int r = 0; r < 8; r++) {
        int32_t y = ys + r, L = 0x7FFF, R = 0x7FFF;
        if ((uint32_t)y < SCREEN_H) { L = BM_LN[2 * y]; R = BM_LN[2 * y + 1]; }
        Lr[r] = L >= 0x7FFF ? 0x7FFFFFF : L <= -0x7FFF ? -0x7FFFFFF : L + gx;
        Rr[r] = R >= 0x7FFF ? 0x7FFFFFF : R <= -0x7FFF ? -0x7FFFFFF : R + gx;
    }
    int32_t cy = ty >> 2;
    int row_ok = ty >= 0 && cy < p->gh;
    const uint16_t *grow = p->grid + cy * p->gw;
    const uint16_t *vrow = VCH(p) + (ty & 3) * 4;
    for (int32_t tx = x0; tx < x1; tx++, d += 2) {
        volatile uint32_t *q = d;
        int32_t cx = tx >> 2;
        if (!row_ok || tx < 0 || cx >= p->gw) {
            for (int k = 0; k < 8; k++, q += CV_P / 4) { q[0] = 0; q[1] = 0; }
            continue;
        }
        uint32_t e = vrow[grow[cx] * 16 + (tx & 3)], bk = e & 0x7FFF;
        const uint32_t *s = tile_ptr(p, bk), *lt = 0;    /* (I12: the dark tile; lt the lit one) */
        for (int r = 0; r < 8; r++, q += CV_P / 4) {
            int32_t rr = (e & 0x8000) ? 7 - r : r;
            int32_t c = Lr[r] - tx * 8, z = Rr[r] - tx * 8 + 1;   /* lit pixels [c, z) of this tile row */
            if (c >= 8 || z <= 0 || c >= z) { q[0] = s[rr * 2]; q[1] = s[rr * 2 + 1]; continue; }   /* (dark) */
            uint32_t m0, m1;
            /* (I12) hard edges like the SNES window: the soft (checkerboard) edges looked like broken tiles */
            if (c <= 0 && z >= 8) m0 = m1 = 0x80808080u;
            else {
                if (c < 0) c = 0;
                if (z > 8) z = 8;
                m0 = bm_ge0[c] & ~bm_ge0[z];
                m1 = bm_ge1[c] & ~bm_ge1[z];
            }
            if (!lt) { lt = tile_ptr_lit(p, bk); s = tile_ptr(p, bk); }   /* (the lit fill may have taken s's slot) */
            if (m0 == 0x80808080u && m1 == 0x80808080u) { q[0] = lt[rr * 2]; q[1] = lt[rr * 2 + 1]; continue; }
            q[0] = bm_mix(s[rr * 2], lt[rr * 2], m0);
            q[1] = bm_mix(s[rr * 2 + 1], lt[rr * 2 + 1], m1);
        }
    }
}
/* (I12) the master's beam history per canvas line (ring of CV_H): the level line it held and the beam's
   edges (canvas x; lit = [L, R]) at the last two pictures - cached, the master's alone */
#define BMH ((int16_t *)((uint8_t *)map_mem + 27840))   /* [CV_H][6]: tag1 L1 R1 tag2 L2 R2 (-> 30912) */
/* lit lanes x >= k of a 4-pixel word (k 0-4) */
static const uint32_t bm_ge4[5] = { 0x80808080u, 0x00808080u, 0x00008080u, 0x00000080u, 0 };
/* (I12) the beam per line: only the pixels between each edge's place now and at the two pictures before
   (this buffer was drawn two pictures ago; the slave's early restore under the old sprites drew with last
   picture's edges).  The master works out the spans (its cached history) into BMS before the band jobs;
   each CPU draws those of its band's rows (cv_band).  (The per-tile-row redraws of the edges were ~250
   tiles a frame while Squawks hovers, on state uncached for both CPUs.) */
#define BMS ((volatile int16_t *)(((uint32_t)map_mem + 17408) | 0x20000000))   /* n, then n x [yl, a0, b0, a1, b1] */
__attribute__((noinline)) static void bm_spans_make(const shared_t *p)
{
    /* (p is the uncached shared state: its fields read once; open / unlit sides as fixed far values -
       the window's edge moving with the camera "changed" every unlit line) */
    int32_t N[4] = { p->cvN[0], p->cvN[1], p->cvN[2], p->cvN[3] }, full = p->cvfull;
    int32_t gx = p->camx - p->ox0, gy = p->camy - p->oy0;
    int32_t xa = -30000, xb = 30000, x0 = N[0] * 8, xe = N[1] * 8;
    const int16_t *ln = bm_shown;          /* (I14: the edges drawn - rate-limited while the beam turns) */
    int16_t *H = BMH;
    volatile int16_t *o = BMS + 1;
    int32_t n = 0;
    for (int32_t yl = N[2] * 8; yl < N[3] * 8; yl++) {
        int32_t y = yl - gy, L = 0x7FFF, R = 0x7FFF;
        if ((uint32_t)y < SCREEN_H) { L = ln[2 * y]; R = ln[2 * y + 1]; }
        L = L >= 0x7FFF ? xb : L <= -0x7FFF ? xa : L + gx;
        R = R >= 0x7FFF ? xb : R <= -0x7FFF ? xa : R + gx;
        int16_t *h = H + (yl & (CV_H - 1)) * 6;
        if (!full && h[3] == (int16_t)yl && (h[4] != L || h[5] != R || (h[0] == (int16_t)yl && (h[1] != L || h[2] != R)))) {
            int32_t s0a = L, s0b = L, s1a = R, s1b = R;
            if (h[4] < s0a) s0a = h[4];
            if (h[4] > s0b) s0b = h[4];
            if (h[5] < s1a) s1a = h[5];
            if (h[5] > s1b) s1b = h[5];
            if (h[0] == (int16_t)yl) {
                if (h[1] < s0a) s0a = h[1];
                if (h[1] > s0b) s0b = h[1];
                if (h[2] < s1a) s1a = h[2];
                if (h[2] > s1b) s1b = h[2];
            }
            if (s0b >= s1a && s1b >= s0a) {   /* (overlapping: one span) */
                if (s1a < s0a) s0a = s1a;
                if (s1b > s0b) s0b = s1b;
                s1a = 1; s1b = 0;
            }
            if (s0a < x0) s0a = x0;
            if (s0b >= xe) s0b = xe - 1;
            if (s1a < x0) s1a = x0;
            if (s1b >= xe) s1b = xe - 1;
            if (s0a <= s0b || s1a <= s1b) {
                o[0] = (int16_t)yl; o[1] = (int16_t)s0a; o[2] = (int16_t)s0b; o[3] = (int16_t)s1a; o[4] = (int16_t)s1b;
                o += 5;
                n++;
            }
        }
        h[3] = h[0]; h[4] = h[1]; h[5] = h[2];
        h[0] = (int16_t)yl; h[1] = (int16_t)L; h[2] = (int16_t)R;
    }
    BMS[0] = (int16_t)n;
}
/* the spans of tile rows [ra, rb) (cv_band, either CPU; this picture's edges in BM_LN).
   (I14) per TILE ROW: each tile's dark and lit versions are fetched once for all the lines of the row
   that need it (copied: the two keys can share a cache slot).  Line by line, a turning beam (spans
   over most of the screen) fetched both versions on every line - in the direct-mapped caches they
   evicted each other and the screen re-baked its tiles 8 times over: 6-10 frames for one picture
   (the "stutter when Squawks turns"). */
__attribute__((noinline)) static void bm_spans_draw(const shared_t *p, int32_t ra, int32_t rb)
{
    int32_t gx = p->camx - p->ox0, gy = p->camy - p->oy0, n = BMS[0];
    const volatile int16_t *e = BMS + 1;
    int32_t i = 0;
    while (i < n) {
        int32_t ty = e[i * 5] >> 3, j = i;
        while (j < n && (e[j * 5] >> 3) == ty) j++;      /* entries [i, j): this tile row's lines */
        int32_t cy = ty >> 2;
        if (ty < ra || ty >= rb || ty < 0 || cy >= p->gh) { i = j; continue; }
        int32_t m = j - i;
        if (m > 8) m = 8;                                  /* (one entry per line) */
        int32_t sa[8][2], sb[8][2], Ls[8], Rs[8], rr[8];
        volatile uint32_t *ln[8];
        int32_t ua = 0x7FFFFFFF, ub = -0x7FFFFFFF;
        for (int32_t k = 0; k < m; k++) {
            const volatile int16_t *q = e + (i + k) * 5;
            int32_t yl = q[0], y = yl - gy, L = 0x7FFF, R = 0x7FFF, xa = -30000, xb = 30000;
            if ((uint32_t)y < SCREEN_H) { L = BM_LN[2 * y]; R = BM_LN[2 * y + 1]; }
            Ls[k] = L >= 0x7FFF ? xb : L <= -0x7FFF ? xa : L + gx;
            Rs[k] = R >= 0x7FFF ? xb : R <= -0x7FFF ? xa : R + gx;
            rr[k] = yl & 7;
            ln[k] = FB32 + (FB_BASE + (yl & (CV_H - 1)) * CV_P + CV_XOFF) / 4;
            for (int h = 0; h < 2; h++) {
                int32_t a = q[1 + 2 * h], b = q[2 + 2 * h];
                sa[k][h] = a & ~3; sb[k][h] = b;           /* 4-pixel groups x with (a & ~3) <= x <= b */
                if (a <= b) { if ((a & ~3) < ua) ua = a & ~3; if (b > ub) ub = b; }
            }
        }
        if (ua > ub) { i = j; continue; }
        const uint16_t *grow = p->grid + cy * p->gw;
        const uint16_t *vrow = VCH(p) + (ty & 3) * 4;
        for (int32_t tx = ua >> 3; tx <= (ub >> 3); tx++) {
            int32_t x8 = tx * 8, cx = tx >> 2;
            int have = 0;                                  /* 0: not fetched, 1: dark, 2: dark + lit */
            uint32_t dk[16], lt[16], te = 0;
            int ok = tx >= 0 && cx < p->gw;
            if (ok) te = vrow[grow[cx] * 16 + (tx & 3)];
            for (int32_t k = 0; k < m; k++) {
                int32_t r = (te & 0x8000) ? 7 - rr[k] : rr[k];
                for (int32_t x = x8; x < x8 + 8; x += 4) {
                    if (!((x >= sa[k][0] && x <= sb[k][0]) || (x >= sa[k][1] && x <= sb[k][1]))) continue;
                    if (!ok) { ln[k][x >> 2] = 0; continue; }
                    if (!have) {
                        const uint32_t *t = tile_ptr(p, te & 0x7FFF);
                        for (int w = 0; w < 16; w++) dk[w] = t[w];
                        have = 1;
                    }
                    uint32_t w = dk[r * 2 + ((x >> 2) & 1)];
                    int32_t c = Ls[k] - x, z = Rs[k] - x + 1;    /* lit lanes [c, z) */
                    uint32_t mk;
                    if (c <= 0 && z >= 4) mk = 0x80808080u;
                    else if (c >= 4 || z <= 0 || c >= z) mk = 0;
                    else { if (c < 0) c = 0; if (z > 4) z = 4; mk = bm_ge4[c] & ~bm_ge4[z]; }
                    if (mk) {
                        if (have < 2) {
                            const uint32_t *t = tile_ptr_lit(p, te & 0x7FFF);
                            for (int v = 0; v < 16; v++) lt[v] = t[v];
                            have = 2;
                        }
                        w = bm_mix(w, lt[r * 2 + ((x >> 2) & 1)], mk);
                    }
                    ln[k][x >> 2] = w;
                }
            }
        }
        i = j;
    }
}

SDRAM_CODE static void render_rect(const shared_t *p, const int32_t *r)
{
    int32_t x0 = r[0], x1 = r[1], y0 = r[2], y1 = r[3];
    int32_t cl = (y0 * 8) % CV_H;
    if (cl < 0) cl += CV_H;
    for (int32_t ty = y0; ty < y1; ty++, cl += 8) {
        if (cl >= CV_H) cl -= CV_H;
        volatile uint32_t *d = FB32 + (FB_BASE + cl * CV_P + CV_XOFF + x0 * 8) / 4;
        if (p->cv >= 2) draw_fog_row(p, d, ty, x0, x1);
        else if (p->bm_game) beamg_row(p, d, ty, x0, x1);
        else if (p->bm_lvl) beam_row(p, d, ty, x0, x1);
        else {
            draw_tile_row(p, d, ty, x0, x1, CV_P / 4);
            if (lamp_img) lamp_row(p, d, ty, x0, x1);
        }
    }
}

/* overlay layer, tile by tile, written through the overwrite region where the hardware skips
   zero bytes (free per-pixel transparency).  Long/word writes; odd destinations use byte+word mix. */
SDRAM_CODE static void put_row8(volatile uint8_t *dst, const uint8_t *src)
{
    uint32_t a = (uint32_t)dst;
    if (!(a & 3)) {
        ((volatile uint32_t *)dst)[0] = ((const uint32_t *)src)[0];
        ((volatile uint32_t *)dst)[1] = ((const uint32_t *)src)[1];
    } else if (!(a & 1)) {
        volatile uint16_t *d = (volatile uint16_t *)dst;
        const uint16_t *q = (const uint16_t *)src;
        d[0] = q[0]; d[1] = q[1]; d[2] = q[2]; d[3] = q[3];
    } else {
        volatile uint16_t *d = (volatile uint16_t *)(dst + 1);
        dst[0] = src[0];
        d[0] = (src[1] << 8) | src[2];
        d[1] = (src[3] << 8) | src[4];
        d[2] = (src[5] << 8) | src[6];
        dst[7] = src[7];
    }
}

static const uint8_t rev8[256] = {
#define R2(n) n, n + 2 * 64, n + 1 * 64, n + 3 * 64
#define R4(n) R2(n), R2(n + 2 * 16), R2(n + 1 * 16), R2(n + 3 * 16)
#define R6(n) R4(n), R4(n + 2 * 4), R4(n + 1 * 4), R4(n + 3 * 4)
    R6(0), R6(2), R6(1), R6(3)
#undef R2
#undef R4
#undef R6
};

#if defined(NOCANVAS) || defined(NOCANVASOV)
#define CV_CODE                      /* reference builds: the canvas overlay code stays in ROM */
SDRAM_VIEWER static void render_overlay(const shared_t *p, int k, int r0, int r1)
{
    const ov_t *o = &ovinfo[k];
    const uint16_t *map = ovmap_ram[k];
    const uint8_t *tiles = (const uint8_t *)tiles_ram;
    int32_t fx = p->camx & 7, fy = p->camy & 7;
    int32_t W8 = o->w * 8, H8 = o->h * 8;
    /* visible buffer lines of this band */
    int32_t ya = r0 * 8, yb = r1 * 8;
    if (ya < fy) ya = fy;
    if (yb > fy + SCREEN_H) yb = fy + SCREEN_H;
    if (ya >= yb) return;
    int32_t OX0 = (p->ovx[k] - fx) & (W8 - 1);          /* overlay x at buffer x 0 */
    int32_t OY0 = p->ovy[k] + 1 - fy;                    /* overlay y at buffer line 0 */
    int32_t by = ya;
    while (by < yb) {
        int32_t oy = (OY0 + by) & (H8 - 1);
        int32_t l0 = oy & 7;
        int32_t nl = 8 - l0;
        if (nl > yb - by) nl = yb - by;
        const uint16_t *mrow = map + (oy >> 3) * o->w;
        int32_t tc = OX0 >> 3;
        int32_t bx = -(OX0 & 7);
        volatile uint8_t *base = OVR8 + FB_BASE + by * STRIDE;
        int32_t al = bx & 3;                 /* STRIDE is a multiple of 4: same alignment for all tiles */
        for (; bx < 328; bx += 8, tc = (tc + 1) & (o->w - 1)) {
            uint32_t e = mrow[tc];
            if (e == 0x7FFF) continue;
            uint32_t slot = e & 0x7FFF;
            uint32_t m = rowmask[slot];
            if (!m) continue;
            const uint8_t *ts = (const uint8_t *)tile_ptr(p, slot);
            uint32_t bits;
            int32_t step;
            const uint8_t *src;
            if (e & 0x8000) { bits = shr8(rev8[m], l0); src = ts + (7 - l0) * 8; step = -8; }
            else            { bits = shr8(m, l0);     src = ts + l0 * 8;       step = 8; }
            bits &= bit32[nl] - 1;
            if (!bits) continue;
            volatile uint8_t *dst = base + bx;
            if (bx < 0 || bx > 320) {
                for (; bits; bits >>= 1, src += step, dst += STRIDE) {
                    if (!(bits & 1)) continue;
                    for (int i = 0; i < 8; i++) {
                        int32_t x = bx + i;
                        if (x >= 0 && x < 328 && src[i]) dst[i] = src[i];
                    }
                }
            } else if (al == 0) {
                for (; bits; bits >>= 1, src += step, dst += STRIDE)
                    if (bits & 1) {
                        ((volatile uint32_t *)dst)[0] = ((const uint32_t *)src)[0];
                        ((volatile uint32_t *)dst)[1] = ((const uint32_t *)src)[1];
                    }
            } else {
                for (; bits; bits >>= 1, src += step, dst += STRIDE)
                    if (bits & 1) put_row8(dst, src);
            }
        }
        by += nl;
    }
}

#endif

/* glimmer tile rows: source pixels masked to every other byte (mk flips per row) */
__attribute__((noinline)) static void glow_rows(volatile uint8_t *dst, const uint8_t *src, int32_t step, uint32_t bits, uint32_t mk)
{
    uint32_t t[2];
    for (; bits; bits >>= 1, src += step, dst += CV_P, mk = ~mk)
        if (bits & 1) {
            t[0] = ((const uint32_t *)src)[0] & mk;
            t[1] = ((const uint32_t *)src)[1] & mk;
            put_row8(dst, (const uint8_t *)t);
        }
}

#ifndef CV_CODE
#define CV_CODE SDRAM_CODE
#endif
/* canvas mode: overlay k on screen lines [y0, y1).  Screen pixel (x, y) is canvas byte
   FB_BASE + ((gy + y) mod CV_H) * CV_P + CV_XOFF + gx + x; all writes stay within x = -7..327, i.e.
   inside the window's columns or the 8-byte gap after them.  Each BG1 tile under a drawn overlay
   row gets its bit set in mask[row & 31] (64 columns, two longs) so it is redrawn before this
   buffer's next overlay pass. */
CV_CODE static void render_overlay_cv(const shared_t *p, int k, int32_t y0, int32_t y1, volatile uint32_t *mask)
{
    const ov_t *o = &ovinfo[k];
    const uint16_t *map = ovmap_ram[k];
    const uint8_t *tiles = (const uint8_t *)tiles_ram;
    int32_t gx = p->camx - p->ox0, gy = p->camy - p->oy0;
    int32_t W8 = o->w * 8, H8 = o->h * 8;
    int32_t OX0 = p->ovx[k] & (W8 - 1);                 /* overlay x at screen x 0 */
    int32_t OY0 = p->ovy[k] + 1;                         /* overlay y at screen line 0 */
    int32_t cl = (gy + y0) % CV_H;
    if (cl < 0) cl += CV_H;
    int32_t sx0 = -(OX0 & 7);
    int32_t al = (gx + CV_XOFF + sx0) & 3;               /* the same for every tile */
    int32_t by = y0;
    while (by < y1) {
        int32_t oy = (OY0 + by) & (H8 - 1);
        int32_t l0 = oy & 7;
        int32_t nl = 8 - l0;
        if (nl > y1 - by) nl = y1 - by;
        if (nl > CV_H - cl) nl = CV_H - cl;             /* runs never cross the canvas wrap */
        const uint16_t *mrow = map + (oy >> 3) * o->w;
        int32_t tc = OX0 >> 3;
        volatile uint8_t *base = OVR8 + FB_BASE + cl * CV_P + CV_XOFF + gx;
        uint32_t ra = (uint32_t)(gy + by) >> 3, rb = (uint32_t)(gy + by + nl - 1) >> 3;
        for (int32_t bx = sx0; bx < 320; bx += 8, tc = (tc + 1) & (o->w - 1)) {
            uint32_t e = mrow[tc];
            if (e == 0x7FFF) continue;
            uint32_t slot = e & 0x7FFF;
            uint32_t m = rowmask[slot];
            if (!m) continue;
            const uint8_t *ts = (const uint8_t *)tile_ptr(p, slot);
            uint32_t bits;
            int32_t step;
            const uint8_t *src;
            if (e & 0x8000) { bits = shr8(rev8[m], l0); src = ts + (7 - l0) * 8; step = -8; }
            else            { bits = shr8(m, l0);     src = ts + l0 * 8;       step = 8; }
            bits &= bit32[nl] - 1;
            if (!bits) continue;
            {   /* mark the BG1 tiles under this run */
                uint32_t ca = (uint32_t)(gx + bx) >> 3, cb = (uint32_t)(gx + bx + 7) >> 3;
                for (uint32_t r = ra; r <= rb; r++) {
                    volatile uint32_t *mr = mask + (r & 31) * 2;
                    mr[(ca >> 5) & 1] |= bit32[ca & 31];
                    mr[(cb >> 5) & 1] |= bit32[cb & 31];
                }
            }
            volatile uint8_t *dst = base + bx;
            if (al == 0) {
                for (; bits; bits >>= 1, src += step, dst += CV_P)
                    if (bits & 1) {
                        ((volatile uint32_t *)dst)[0] = ((const uint32_t *)src)[0];
                        ((volatile uint32_t *)dst)[1] = ((const uint32_t *)src)[1];
                    }
            } else {
                for (; bits; bits >>= 1, src += step, dst += CV_P)
                    if (bits & 1) put_row8(dst, src);
            }
        }
        by += nl;
        cl += nl;
        if (cl >= CV_H) cl -= CV_H;
    }
}

/* the Slipslide glimmer: render_overlay_cv drawing a 50% checkerboard (ROM: it runs a few frames at a time) */
__attribute__((noinline)) static void render_overlay_glow(const shared_t *p, int k, int32_t y0, int32_t y1, volatile uint32_t *mask)
{
    const ov_t *o = &ovinfo[k];
    const uint16_t *map = ovmap_ram[k];
    const uint8_t *tiles = (const uint8_t *)tiles_ram;
    int32_t gx = p->camx - p->ox0, gy = p->camy - p->oy0;
    int32_t W8 = o->w * 8, H8 = o->h * 8;
    int32_t OX0 = p->ovx[k] & (W8 - 1);                 /* overlay x at screen x 0 */
    int32_t OY0 = p->ovy[k] + 1;                         /* overlay y at screen line 0 */
    int32_t cl = (gy + y0) % CV_H;
    if (cl < 0) cl += CV_H;
    int32_t sx0 = -(OX0 & 7);
    int32_t by = y0;
    while (by < y1) {
        int32_t oy = (OY0 + by) & (H8 - 1);
        int32_t l0 = oy & 7;
        int32_t nl = 8 - l0;
        if (nl > y1 - by) nl = y1 - by;
        if (nl > CV_H - cl) nl = CV_H - cl;             /* runs never cross the canvas wrap */
        const uint16_t *mrow = map + (oy >> 3) * o->w;
        int32_t tc = OX0 >> 3;
        volatile uint8_t *base = OVR8 + FB_BASE + cl * CV_P + CV_XOFF + gx;
        uint32_t ra = (uint32_t)(gy + by) >> 3, rb = (uint32_t)(gy + by + nl - 1) >> 3;
        for (int32_t bx = sx0; bx < 320; bx += 8, tc = (tc + 1) & (o->w - 1)) {
            uint32_t e = mrow[tc];
            if (e == 0x7FFF) continue;
            uint32_t slot = e & 0x7FFF;
            uint32_t m = rowmask[slot];
            if (!m) continue;
            const uint8_t *ts = (const uint8_t *)tile_ptr(p, slot);
            uint32_t bits;
            int32_t step;
            const uint8_t *src;
            if (e & 0x8000) { bits = shr8(rev8[m], l0); src = ts + (7 - l0) * 8; step = -8; }
            else            { bits = shr8(m, l0);     src = ts + l0 * 8;       step = 8; }
            bits &= bit32[nl] - 1;
            if (!bits) continue;
            {   /* mark the BG1 tiles under this run */
                uint32_t ca = (uint32_t)(gx + bx) >> 3, cb = (uint32_t)(gx + bx + 7) >> 3;
                for (uint32_t r = ra; r <= rb; r++) {
                    volatile uint32_t *mr = mask + (r & 31) * 2;
                    mr[(ca >> 5) & 1] |= bit32[ca & 31];
                    mr[(cb >> 5) & 1] |= bit32[cb & 31];
                }
            }
            volatile uint8_t *dst = base + bx;
            /* every other pixel in level space, the other half in the other buffer */
            glow_rows(dst, src, step, bits, (((uint32_t)dst + gy + by + p->cvbi) & 1) ? 0x00FF00FF : 0xFF00FF00);
        }
        by += nl;
        cl += nl;
        if (cl >= CV_H) cl -= CV_H;
    }
}

/* heavy snow overlay: per screen line, write only that overlay row's flake pixels (precomputed
   per animation frame), with the per-line vertical wobble and the 256-pixel horizontal wrap */
SDRAM_VIEWER static void render_overlay_wobble(const shared_t *p, int k, int r0, int r1)
{
    int32_t fx = p->camx & 7, fy = p->camy & 7;
    int32_t ya = r0 * 8, yb = r1 * 8;
    if (ya < fy) ya = fy;
    if (yb > fy + SCREEN_H) yb = fy + SCREEN_H;
    const uint8_t *blk = flake_frame[p->anim_cur[k]];
    const uint16_t *idx = (const uint16_t *)blk;
    const uint8_t *pairs = blk + 257 * 2;
    int32_t ovx = p->ovx[k], ovy = p->ovy[k], ph = p->wob_phase;
    for (int32_t by = ya; by < yb; by++) {
        int32_t sy = by - fy;
        int32_t oy = (ovy + sy + 1 + snow_wob[(sy + ph) & 31]) & 255;
        uint32_t n0 = idx[oy], n1 = idx[oy + 1];
        if (n0 == n1) continue;
        volatile uint8_t *line = FB8 + FB_BASE + by * STRIDE + fx;
        const uint8_t *q = pairs + n0 * 2;
        for (uint32_t n = n1 - n0; n; n--, q += 2) {
            uint32_t sx = (q[0] - ovx) & 255;
            uint8_t c = q[1];
            line[sx] = c;
            if (sx < 64) line[sx + 256] = c;
        }
    }
}

/* ---- comp mode: line compositor ----
   Screen lines are composed at the SNES's native 256-pixel width.  The whole line is displayed
   shifted by P = light x & 3 pixels (line table + SFT), which puts the light layer on long
   boundaries.  The inner loops (sh/comp.s, generated by tools/gen_comp_asm.py) stream the base layer
   with a fixed byte shift (no barrel shifter on the SH-2), merge BG1 from a line buffer in on-chip
   RAM when the row has any (bit-trick transparency mask; BG1 gets the underwater ripple), add the
   light layer's palette-bank offsets and write two framebuffer longs per light tile.  34 light tiles
   are written per line; the overhang beyond the 256-pixel window is hidden by black Genesis plane A
   cells (the comp palette has no priority bit). */
#define COMP_X0    32                 /* first composited screen x */
#define ONCHIP     ((uint32_t *)0xC0000000)
#define NT         38                 /* tiles per row setup */
/* comp levels store tiles row-pair-interleaved: tile s, row r at
   tiles_ram + (r >> 1) * COMP_RP + s * 16 + (r & 1) * 8, so the row pairs of consecutive slots fall in
   consecutive cache sets (linear 64-byte tiles put a given row of every tile in only 16 of the 64 sets).
   A v-flipped row is at COMP_FLIPC - offset: entries store tile + COMP_FLIPC + 1 and xor ~0 (= -o - 1). */
#define COMP_RP    (MAX_SLOTS * 16)
#define COMP_FLIPC (3 * COMP_RP + 8)
#define ZERO_SLOT  (MAX_SLOTS - 1)
#define TILE_B     ((uint32_t)tiles_ram)
static inline uint32_t comp_roff(int r) { return (r >> 1) * COMP_RP + (r & 1) * 8; }

typedef struct {
    uint32_t ent[NT * 2];             /* per tile: row-pair-0 address (+ COMP_FLIPC + 1 if v-flipped), xor */
} lrow_t;
/* on-chip RAM (each SH-2 has its own): per-pair BG1 classes, per-tile BG1 classes, then the
   base, BG1 and light row setups */
#define COMP_CLS   ((uint8_t *)ONCHIP)
#define COMP_TCLS  ((uint8_t *)(ONCHIP + 16))
#define COMP_ROWS  ((lrow_t *)(ONCHIP + 72))
typedef void comp_fn(volatile uint32_t *, const uint32_t *, const uint32_t *, const uint32_t *);
extern comp_fn comp_k0_b0, comp_k0_b1, comp_k0_b2, comp_k0_b3, comp_k1_b0, comp_k1_b1, comp_k1_b2, comp_k1_b3;
extern comp_fn comp_mix_00_00, comp_mix_00_01, comp_mix_00_02, comp_mix_00_03, comp_mix_00_10, comp_mix_00_11, comp_mix_00_12, comp_mix_00_13, comp_mix_01_00, comp_mix_01_01, comp_mix_01_02, comp_mix_01_03, comp_mix_01_10, comp_mix_01_11, comp_mix_01_12, comp_mix_01_13, comp_mix_02_00, comp_mix_02_01, comp_mix_02_02, comp_mix_02_03, comp_mix_02_10, comp_mix_02_11, comp_mix_02_12, comp_mix_02_13, comp_mix_03_00, comp_mix_03_01, comp_mix_03_02, comp_mix_03_03, comp_mix_03_10, comp_mix_03_11, comp_mix_03_12, comp_mix_03_13, comp_mix_10_00, comp_mix_10_01, comp_mix_10_02, comp_mix_10_03, comp_mix_10_10, comp_mix_10_11, comp_mix_10_12, comp_mix_10_13, comp_mix_11_00, comp_mix_11_01, comp_mix_11_02, comp_mix_11_03, comp_mix_11_10, comp_mix_11_11, comp_mix_11_12, comp_mix_11_13, comp_mix_12_00, comp_mix_12_01, comp_mix_12_02, comp_mix_12_03, comp_mix_12_10, comp_mix_12_11, comp_mix_12_12, comp_mix_12_13, comp_mix_13_00, comp_mix_13_01, comp_mix_13_02, comp_mix_13_03, comp_mix_13_10, comp_mix_13_11, comp_mix_13_12, comp_mix_13_13;
static comp_fn *const comp_tab[8] = {
    comp_k0_b0, comp_k0_b1, comp_k0_b2, comp_k0_b3, comp_k1_b0, comp_k1_b1, comp_k1_b2, comp_k1_b3,
};
static comp_fn *const mix_tab[64] = {   /* [base k*4+b][bg1 k*4+b] */
    comp_mix_00_00, comp_mix_00_01, comp_mix_00_02, comp_mix_00_03, comp_mix_00_10, comp_mix_00_11, comp_mix_00_12, comp_mix_00_13,
    comp_mix_01_00, comp_mix_01_01, comp_mix_01_02, comp_mix_01_03, comp_mix_01_10, comp_mix_01_11, comp_mix_01_12, comp_mix_01_13,
    comp_mix_02_00, comp_mix_02_01, comp_mix_02_02, comp_mix_02_03, comp_mix_02_10, comp_mix_02_11, comp_mix_02_12, comp_mix_02_13,
    comp_mix_03_00, comp_mix_03_01, comp_mix_03_02, comp_mix_03_03, comp_mix_03_10, comp_mix_03_11, comp_mix_03_12, comp_mix_03_13,
    comp_mix_10_00, comp_mix_10_01, comp_mix_10_02, comp_mix_10_03, comp_mix_10_10, comp_mix_10_11, comp_mix_10_12, comp_mix_10_13,
    comp_mix_11_00, comp_mix_11_01, comp_mix_11_02, comp_mix_11_03, comp_mix_11_10, comp_mix_11_11, comp_mix_11_12, comp_mix_11_13,
    comp_mix_12_00, comp_mix_12_01, comp_mix_12_02, comp_mix_12_03, comp_mix_12_10, comp_mix_12_11, comp_mix_12_12, comp_mix_12_13,
    comp_mix_13_00, comp_mix_13_01, comp_mix_13_02, comp_mix_13_03, comp_mix_13_10, comp_mix_13_11, comp_mix_13_12, comp_mix_13_13,
};
#ifdef PROF
static uint32_t prof[5], prof_nslots;
#define PT(k) do { if (!cpu) { uint16_t t_ = frt_read(); prof[k] += (uint16_t)(t_ - tp); tp = t_; } } while (0)
#else
#define PT(k) do { } while (0)
#endif

extern void comp_prep(uint32_t *ent, const uint32_t *src, uint32_t n);
extern void comp_classes(uint8_t *c, const uint8_t *tc, uint32_t go_nonzero);
#define comp_map map_mem              /* comp levels: base/light maps as tile row-pair-0 addresses (odd = v-flip) */

/* NT entries of a map row from tile tc (wrapping at w) */
SDRAM_VIEWER static void lrow_prep(lrow_t *L, const uint32_t *mrow, uint32_t w, uint32_t tc)
{
    uint32_t *e = L->ent, left = NT;
    while (left) {
        uint32_t n = w - tc;
        if (n > left) n = left;
        comp_prep(e, mrow + tc, n);
        e += n * 2; left -= n; tc = 0;
    }
}

/* BG1 tile ring (comp levels): 64x32 level tiles around the camera in entry form (row-pair-0
   address, odd = v-flip) plus a class byte (0 empty, 1 partial, 2 opaque).  The master streams in
   new columns/rows before each frame's token; both CPUs read it through the cache-through alias
   (the slave's cache could hold stale lines).  Comp vch entries: bit 15 v-flip, bit 14 opaque. */
#define RING_W 64
#define RING_H 32
uint32_t ring_ent[RING_H][RING_W];    /* (outside comp levels: the game's WRAM pages of class 16, mkgame.py) */
static uint8_t ring_cls[RING_H][RING_W];
static int32_t ring_x0 = -0x10000, ring_y0 = -0x10000;

static void ring_fill(const shared_t *p, int32_t tx, int32_t ty)
{
    int32_t cx = tx >> 2, cy = ty >> 2;
    uint32_t e = ZERO_SLOT;
    if (ty >= 0 && cy < p->gh && cx >= 0 && cx < p->gw)
        e = VCH(p)[p->grid[cy * p->gw + cx] * 16 + (ty & 3) * 4 + (tx & 3)];
    uint32_t slot = e & 0x3FFF;
    ring_ent[ty & (RING_H - 1)][tx & (RING_W - 1)] = TILE_B + slot * 16 + ((e & 0x8000) ? COMP_FLIPC + 1 : 0);
    ring_cls[ty & (RING_H - 1)][tx & (RING_W - 1)] = slot == ZERO_SLOT ? 0 : (e & 0x4000) ? 2 : 1;
}

/* make tiles [txa, txa + 40) x [tya, tya + 30) valid */
static void ring_update(const shared_t *p, int32_t txa, int32_t tya)
{
    int32_t nx = ring_x0, ny = ring_y0;
    if (txa < nx) nx = txa - 8;
    else if (txa + 40 > nx + RING_W) nx = txa + 40 - RING_W + 8;
    if (tya < ny) ny = tya - 1;
    else if (tya + 30 > ny + RING_H) ny = tya + 30 - RING_H + 1;
    if (ny != ring_y0) {
        int32_t a = ny, b = ny + RING_H;                 /* rows to (re)fill: new ones only */
        if (ny > ring_y0 && ny < ring_y0 + RING_H) a = ring_y0 + RING_H;
        else if (ny < ring_y0 && ny + RING_H > ring_y0) b = ring_y0;
        for (int32_t ty = a; ty < b; ty++)
            for (int32_t tx = nx; tx < nx + RING_W; tx++) ring_fill(p, tx, ty);
    }
    if (nx != ring_x0) {
        int32_t a = nx, b = nx + RING_W;
        if (nx > ring_x0 && nx < ring_x0 + RING_W) a = ring_x0 + RING_W;
        else if (nx < ring_x0 && nx + RING_W > ring_x0) b = ring_x0;
        for (int32_t ty = ny; ty < ny + RING_H; ty++)
            for (int32_t tx = a; tx < b; tx++) ring_fill(p, tx, ty);
    }
    ring_x0 = nx;
    ring_y0 = ny;
}

/* BG1 tile row ty from tile tx: entries + tile classes (COMP_TCLS); returns any non-empty */
SDRAM_VIEWER static int lrow_prep_bg1(lrow_t *L, const shared_t *p, int32_t ty, int32_t tx)
{
    const uint32_t *er = UNCACHED(&ring_ent[ty & (RING_H - 1)][0]);
    const uint8_t *cr = UNCACHED(&ring_cls[ty & (RING_H - 1)][0]);
    uint8_t *tc = COMP_TCLS;
    uint32_t c = tx & (RING_W - 1), any = 0;
    (void)p;
    lrow_prep(L, er, RING_W, c);
    for (int i = 0; i < NT; i++, c = (c + 1) & (RING_W - 1)) any |= tc[i] = cr[c];
    return any != 0;
}

/* per-CPU band state in on-chip RAM, shared with comp_band (sh/comp.s, CS dict in the generator) */
#define CST        (ONCHIP + 300)
enum { CS_ROFF = 0, CS_DSTROW = 8, CS_SY, CS_SYEND, CS_OVYB1, CS_BH, CS_BKEY, CS_LKEY, CS_WAVE,
       CS_LW1, CS_GY0, CS_GX0C, CS_GMROW, CS_GTX, CS_GANY, CS_GCX, CS_PAT, CS_RPH, CS_BX,
       CS_MIXTAB, CS_CBBASE, CS_CBLIGHT, CS_CBBG1, CS_PLAINTAB, CS_TDST,
       CS_ARGS = 32, CS_PARGS = 37, CS_BW1 = 41, CS_LH, CS_FNCLS, CS_STRIDE, CS_TVAR,
       CS_BMAP = 48, CS_BW, CS_LMAP, CS_LW, CS_P, CS_N };
extern void comp_band(uint32_t *st);
static const uint8_t zero_pat[64];

/* light offsets per 4-line band (comp_wave, defined with the HUD): {H = light x at screen x 0,
   V = light row at screen line 0} */

SDRAM_VIEWER static void cb_base(int32_t maprow, uint32_t tile)
{
    uint32_t *st = CST;
    lrow_prep(COMP_ROWS, (const uint32_t *)st[CS_BMAP] + maprow * st[CS_BW], st[CS_BW], tile);
}

SDRAM_VIEWER static void cb_light(int32_t maprow, uint32_t tile)
{
    uint32_t *st = CST;
    lrow_prep(COMP_ROWS + 2, (const uint32_t *)st[CS_LMAP] + maprow * st[CS_LW], st[CS_LW], tile);
}

SDRAM_VIEWER static int cb_bg1(int32_t ty, int32_t tx)
{
#ifdef COMP_NOBG1
    (void)ty; (void)tx;
    return 0;
#endif
    if (0)                                  /* (old performance mode: BG1 drawn afterwards, unfogged) */
        return 0;
    return lrow_prep_bg1(COMP_ROWS + 1, (const shared_t *)CST[CS_P], ty, tx);
}

SDRAM_VIEWER static void render_comp(const shared_t *p, int sy0, int sy1, int cpu)
{
    const ov_t *ob = &ovinfo[0], *ol = &ovinfo[1];
    uint32_t *st = CST;
    (void)cpu;
    for (int r = 0; r < 8; r++) st[CS_ROFF + r] = comp_roff(r);
    st[CS_DSTROW] = (uint32_t)FB32 + FB_BASE + sy0 * STRIDE + COMP_X0;
    st[CS_SY] = sy0;
    st[CS_SYEND] = sy1;
    st[CS_OVYB1] = p->ovy[0] + 1;
    st[CS_BH] = ob->h * 8 - 1;
    st[CS_BKEY] = st[CS_LKEY] = 0x80000000u;
    st[CS_WAVE] = (uint32_t)UNCACHED(&comp_wave[0][0]);
    st[CS_LW1] = ol->w * 8 - 1;
    st[CS_GY0] = p->camy - p->oy0;
    st[CS_GX0C] = p->camx - p->ox0;
    st[CS_GMROW] = 0x80000000u;
    st[CS_GTX] = 0;
    st[CS_GANY] = 0;
    st[CS_GCX] = 0x80000000u;
    st[CS_PAT] = (uint32_t)(ripple ? ripple_pat : zero_pat);
    st[CS_RPH] = p->ripple_phase;
    st[CS_BX] = p->ovx[0];
    st[CS_MIXTAB] = (uint32_t)mix_tab;
    st[CS_CBBASE] = (uint32_t)cb_base;
    st[CS_CBLIGHT] = (uint32_t)cb_light;
    st[CS_CBBG1] = (uint32_t)cb_bg1;
    st[CS_PLAINTAB] = (uint32_t)comp_tab;
    st[CS_ARGS + 3] = (uint32_t)COMP_CLS;
    st[CS_PARGS + 2] = 0;
    st[CS_PARGS + 3] = 33;   /* 33 light tiles = 66 longs cover the 256-pixel window at any phase */
    st[CS_BW1] = ob->w * 8 - 1;
    st[CS_LH] = ol->h * 8 - 1;
    st[CS_FNCLS] = (uint32_t)comp_classes;
    st[CS_STRIDE] = STRIDE;
    st[CS_BMAP] = (uint32_t)comp_map[0];
    st[CS_BW] = ob->w;
    st[CS_LMAP] = (uint32_t)comp_map[1];
    st[CS_LW] = ol->w;
    st[CS_P] = (uint32_t)p;
#ifdef PROF
    uint16_t t0 = frt_read();
    for (int k = 56; k < 66; k++) st[k] = 0;
#endif
    comp_band(st);
#ifdef PROF
    if (!cpu) {
        prof[0] = (uint16_t)(frt_read() - t0);
        prof[1] = st[57] + st[58];
        prof[2] = st[59] + st[60];
        prof[3] = st[61] + st[62];
        prof[4] = st[63] * 100 / (st[63] + st[64] + st[65] + 1) * 1000 + st[65] * 100 / (st[63] + st[64] + st[65] + 1) * 10;
    }
#endif
}

/* comp performance mode: BG1 drawn over the fogged background through the overwrite region (zero
   bytes are skipped by the hardware).  Screen x shows FB x + P (P = per-band line shift). */
SDRAM_VIEWER static void render_bg1_ovr(const shared_t *p, int32_t y0, int32_t y1)
{
    int32_t lx = p->camx - p->ox0, ly = p->camy - p->oy0;
    const uint8_t *tb = (const uint8_t *)TILE_B;
    const int32_t *cw = UNCACHED(&comp_wave[0][0]);
    int32_t y = y0;
    while (y < y1) {
        int32_t wy = ly + y, ty = wy >> 3, row = wy & 7, cy = ty >> 2;
        int32_t nl = 8 - row;
        if (nl > y1 - y) nl = y1 - y;
        if (ty < 0 || cy >= p->gh) { y += nl; continue; }
        const uint16_t *grow = p->grid + cy * p->gw;
        const uint16_t *vrow = VCH(p) + (ty & 3) * 4;
        int32_t x = COMP_X0 - ((lx + COMP_X0) & 7);
        int32_t tx = (lx + x) >> 3;
        for (; x < COMP_X0 + 256; x += 8, tx++) {
            int32_t cx = tx >> 2;
            if (tx < 0 || cx >= p->gw) continue;
            uint32_t e = vrow[grow[cx] * 16 + (tx & 3)];
            uint32_t slot = e & 0x3FFF;
            if (slot == ZERO_SLOT) continue;
            uint32_t m = rowmask[slot];
            if (!m) continue;
            const uint8_t *ts = tb + slot * 16;
            for (int32_t l = 0; l < nl; l++) {
                int32_t sr = (e & 0x8000) ? 7 - (row + l) : row + l;
                if (!((m >> sr) & 1)) continue;
                int32_t yy = y + l;
                volatile uint8_t *dst = OVR8 + FB_BASE + yy * STRIDE + x + (cw[(yy >> 2) * 2] & 3);
                put_row8(dst, ts + comp_roff(sr));
            }
        }
        y += nl;
    }
}

/* canvas mode, one CPU's share: grid tile rows [ra, rb) of the window - the tiles that entered the
   window, the ones under this buffer's last HUD and overlay pass, then the overlay on these rows'
   screen lines (marking the BG1 tiles it covers in 'mask') */
/* temple millstone: canvas tiles the next picture will cover completely are not restored early
   (predicted position, from the last two frames); they go to ms_defer and cv_band restores the
   ones the real position leaves uncovered (sprites.c spr_ms_cover) */
#define ms_defer ((uint32_t (*)[32][2])((uint8_t *)map_mem + 17408))   /* (temple: no rain / overlays) */
static volatile int32_t ms_lvl;          /* (uncached) this level has the millstone: ms_defer in use */
extern volatile int32_t spr_ms_qpos[2][3], spr_ms_cq[4];
extern int spr_ms_cover(int32_t qx, int32_t qy, int32_t k, int32_t tx, int32_t ty);
extern void spr_ms_sync(void);
static void cv_rect_rows(const shared_t *p, int32_t x0, int32_t x1, int32_t y0, int32_t y1, int32_t ra, int32_t rb)
{
    if (y0 < ra) y0 = ra;
    if (y1 > rb) y1 = rb;
    if (x0 >= x1 || y0 >= y1) return;
    int32_t r[4] = { x0, x1, y0, y1 };
    render_rect(p, r);
}

__attribute__((noinline)) static void cv_band(const shared_t *p, int32_t ra, int32_t rb, volatile uint32_t *mask)
{
    const int32_t *N = p->cvN, *I = p->cvI, *H = p->cvH;
    /* this CPU's overlay marks: cached (write-through, so the master can read the slave's
       uncached after the sync) - uncached read-modify-writes cost ~25 cycles each on MiSTer */
    if (p->ovcap)
        for (int i = 0; i < 64; i++) mask[i] = 0;
    if (p->cvfull) {
        cv_rect_rows(p, N[0], N[1], N[2], N[3], ra, rb);
    } else {
        cv_rect_rows(p, N[0], I[0], N[2], N[3], ra, rb);                  /* new columns */
        cv_rect_rows(p, I[1], N[1], N[2], N[3], ra, rb);
        cv_rect_rows(p, I[0], I[1], N[2], I[2], ra, rb);                  /* new rows */
        cv_rect_rows(p, I[0], I[1], I[3], N[3], ra, rb);
        cv_rect_rows(p, H[0] > I[0] ? H[0] : I[0], H[1] < I[1] ? H[1] : I[1],   /* under the HUD */
                     H[2] > I[2] ? H[2] : I[2], H[3] < I[3] ? H[3] : I[3], ra, rb);
        if (p->spr) {
            /* under this buffer's last sprites */
            int32_t y0 = I[2] > ra ? I[2] : ra, y1 = I[3] < rb ? I[3] : rb;
            const uint32_t (*sm)[2] = (const uint32_t (*)[2])((uint32_t)&spr_marks[p->cvbi][0][0] | 0x20000000);
            uint32_t (*df)[2] = (uint32_t (*)[2])((uint32_t)&ms_defer[p->cvbi][0][0] | 0x20000000);
            const volatile int32_t *cq = UNCACHED(&spr_ms_cq[0]);
            int cov = cq[2], lvl = *UNCACHED(&ms_lvl);
            int32_t cqx = cq[0], cqy = cq[1], cqk = cq[3];
            if (cov) spr_ms_sync();             /* (spr_ms_cover reads the masks cached) */
            for (int32_t ty = y0; ty < y1; ty++) {
                const uint32_t *m = sm[ty & 31];
                uint32_t m0 = m[0], m1 = m[1];
                if (lvl) { m0 |= df[ty & 31][0]; m1 |= df[ty & 31][1]; df[ty & 31][0] = df[ty & 31][1] = 0; }
                if (p->rip) { m0 |= sm[(ty - 1) & 31][0]; m1 |= sm[(ty - 1) & 31][1]; }
                if (!(m0 | m1)) continue;
                int32_t run = 0x7FFFFFFF;
                for (int32_t tx = I[0]; tx <= I[1]; tx++) {
                    int on = tx < I[1] && ((((tx & 32) ? m1 : m0) & bit32[tx & 31]) != 0);
                    if (on && cov && spr_ms_cover(cqx, cqy, cqk, tx, ty)) on = 0;
                    if (on) { if (run == 0x7FFFFFFF) run = tx; }
                    else if (run != 0x7FFFFFFF) {
                        int32_t r[4] = { run, tx, ty, ty + 1 };
                        render_rect(p, r);
                        run = 0x7FFFFFFF;
                    }
                }
            }
        }
        if (p->ovcap) {
            /* under this buffer's last overlay pass: runs per tile row */
            int32_t y0 = I[2] > ra ? I[2] : ra, y1 = I[3] < rb ? I[3] : rb;
            const uint32_t (*ovd)[2] = (const uint32_t (*)[2])((uint32_t)&cv_ovd[p->cvbi][0][0] | 0x20000000);
            for (int32_t ty = y0; ty < y1; ty++) {
                const uint32_t *m = ovd[ty & 31];
                uint32_t m0 = m[0], m1 = m[1];
                if (!(m0 | m1)) continue;
                int32_t run = 0x7FFFFFFF;
                for (int32_t tx = I[0]; tx <= I[1]; tx++) {
                    int on = tx < I[1] && ((((tx & 32) ? m1 : m0) & bit32[tx & 31]) != 0);
                    if (on) { if (run == 0x7FFFFFFF) run = tx; }
                    else if (run != 0x7FFFFFFF) {
                        int32_t r[4] = { run, tx, ty, ty + 1 };
                        render_rect(p, r);
                        run = 0x7FFFFFFF;
                    }
                }
            }
        }
    }
    if (p->bm_game) bm_spans_draw(p, ra, rb);
    else if (p->bm_lvl) beam_pass(p, ra, rb);
    if (p->nov) {
        int32_t gy = p->camy - p->oy0;
        int32_t l0 = ra * 8 - gy, l1 = rb * 8 - gy;
        if (l0 < 0) l0 = 0;
        if (l1 > SCREEN_H) l1 = SCREEN_H;
        if (p->glow) {
            render_overlay_glow(p, p->cvbi, l0 < l1 ? l0 : l1, l1, mask);
        }
        else
            for (int k = 0; k < p->nov; k++)
                if (l0 < l1) render_overlay_cv(p, k, l0, l1, mask);
    }
}

/* job 3, overlay levels: put the canvas back under buffer bi's last overlay pass (inside its valid
   window) and clear those marks, so the master's cv_band after the logic only draws the new pass */
__attribute__((noinline)) static void cv_restore_ov(const shared_t *p)
{
    const int32_t *W = p->pw;
    uint32_t (*ovd)[2] = (uint32_t (*)[2])((uint32_t)&cv_ovd[p->pbi][0][0] | 0x20000000);
    int32_t y1 = W[2] + p->ov_rows;
    if (y1 > W[3]) y1 = W[3];
    for (int32_t ty = W[2]; ty < y1; ty++) {
        uint32_t *m = ovd[ty & 31];
        uint32_t m0 = m[0], m1 = m[1];
        if (!(m0 | m1)) continue;
        int32_t run = 0x7FFFFFFF;
        for (int32_t tx = W[0]; tx <= W[1]; tx++) {
            int on = tx < W[1] && ((((tx & 32) ? m1 : m0) & bit32[tx & 31]) != 0);
            if (on) { if (run == 0x7FFFFFFF) run = tx; }
            else if (run != 0x7FFFFFFF) {
                int32_t r[4] = { run, tx, ty, ty + 1 };
                render_rect(p, r);
                run = 0x7FFFFFFF;
            }
        }
        m[0] = m[1] = 0;
    }
}

/* job 3 (the slave, while the master runs the game logic): put the canvas back under the sprites
   last drawn into buffer bi, inside its valid window, and clear those marks */
__attribute__((noinline)) static void cv_restore_spr(const shared_t *p)
{
    const int32_t *W = p->pw;
    uint32_t (*sm)[2] = (uint32_t (*)[2])((uint32_t)&spr_marks[p->pbi][0][0] | 0x20000000);
    uint32_t (*df)[2] = (uint32_t (*)[2])((uint32_t)&ms_defer[p->pbi][0][0] | 0x20000000);
    const volatile int32_t *qa = UNCACHED(&spr_ms_qpos[p->pbi][0]), *qb = UNCACHED(&spr_ms_qpos[p->pbi ^ 1][0]);
    int lvl = *UNCACHED(&ms_lvl), pred = lvl && qa[2] && qb[2];
    int32_t pqx = 2 * qb[0] - qa[0], pqy = 2 * qb[1] - qa[1];
    extern int32_t spr_ms_c0, spr_ms_r0, spr_ms_c1, spr_ms_r1;
    int32_t bx0 = *UNCACHED(&spr_ms_c0) * 8 - 16, bx1 = *UNCACHED(&spr_ms_c1) * 8 + 16;
    int32_t by0 = *UNCACHED(&spr_ms_r0) * 8 - 16, by1 = *UNCACHED(&spr_ms_r1) * 8 + 16;
    if (lvl) for (int i = 0; i < 32; i++) df[i][0] = df[i][1] = 0;
    for (int32_t ty = W[2]; ty < W[3]; ty++) {
        uint32_t *m = sm[ty & 31];
        uint32_t m0 = m[0], m1 = m[1];
        if (!(m0 | m1)) continue;
        int32_t run = 0x7FFFFFFF;
        for (int32_t tx = W[0]; tx <= W[1]; tx++) {
            int on = tx < W[1] && ((((tx & 32) ? m1 : m0) & bit32[tx & 31]) != 0);
            /* (all of the predicted picture's area is deferred: its restore then runs in the band
               phase, split between both CPUs, instead of the slave alone while the master waits) */
            if (on && pred && tx * 8 + 8 > pqx + bx0 && tx * 8 < pqx + bx1 && ty * 8 + 8 > pqy + by0 && ty * 8 < pqy + by1) {
                df[ty & 31][(tx >> 5) & 1] |= bit32[tx & 31]; on = 0; }
            if (on) { if (run == 0x7FFFFFFF) run = tx; }
            else if (run != 0x7FFFFFFF) {
                int32_t r[4] = { run, tx, ty, ty + 1 };
                render_rect(p, r);
                run = 0x7FFFFFFF;
            }
        }
    }
    for (int i = 0; i < 32; i++) sm[i][0] = sm[i][1] = 0;
}

SDRAM_CODE static void render_band(const shared_t *p, int r0, int r1)
{
    if (comp) {
        int32_t y0 = r0 == 0 ? 0 : p->split, y1 = r0 == 0 ? p->split : SCREEN_H;
        render_comp(p, y0, y1, r0 != 0);
        if (0) render_bg1_ovr(p, y0, y1);
        return;
    }
    if (p->cv) {                         /* canvas mode: the slave's rows, if any */
        if (r0 != 0 && p->cvrows[0] < p->cvrows[1])
            cv_band(p, p->cvrows[0], p->cvrows[1], &cv_smask[0][0]);
        return;
    }
    render_rows(p, r0, r1);
    for (int k = 0; k < p->nov; k++) {
        if (ovinfo[k].xm == 4)
            render_overlay_wobble(p, k, r0, r1);
#if defined(NOCANVAS) || defined(NOCANVASOV)
        else
            render_overlay(p, k, r0, r1);
#endif
    }
}

/* ------------------------------------------------------------ text */
static const uint8_t *font;

static int text_width(const char *s)
{
    int w = 0;
    for (; *s; s++) {
        int c = *s - 32;
        if (c < 0 || c > 95) c = 0;
        w += font[c * 11];
    }
    return w;
}

/* draws text into the back buffer at screen coords with a 1px shadow (colour 254) */
static void text(int x, int y, const char *s, uint8_t colour)
{
    for (; *s; s++) {
        int c = *s - 32;
        if (c < 0 || c > 95) c = 0;
        const uint8_t *g = font + c * 11;
        for (int r = 0; r < 10; r++) {
            uint8_t bits = g[1 + r];
            for (int b = 0; b < 8; b++) {
                if (bits & (0x80 >> b)) {
                    uint32_t o = FB_BASE + (uint32_t)(y + r) * STRIDE + x + b;
                    FB8[o + STRIDE + 1] = 254;
                    FB8[o] = colour;
                }
            }
        }
        x += g[0];
    }
}

static void box(int x, int y, int w, int h, uint8_t colour)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            FB8[FB_BASE + (uint32_t)(y + j) * STRIDE + x + i] = colour;
}

/* small HUD font (3x5 at 2x) kept from v1 for the in-level readout */
static const char hud_chars[] = "0123456789FPSMX:%/ YG";
static const uint16_t hud_bits[] = {
    0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF,
    0x79A4, 0x7BE4, 0x79CF, 0x5FED, 0x5AAD, 0x0410, 0x52A5, 0x12A4, 0x0000, 0x5AA4, 0x396B,
};
/* (I21c) the pause menu's STATS line in the game: bottom-left, out of the HUD's way */
#define HUD_Y (SCREEN_H - 14)


static void hud_text(int sx, int sy, int fx, int fy, const char *s)
{
    for (; *s; s++, sx += 8) {
        int gi = 0;
        while (hud_chars[gi] && hud_chars[gi] != *s) gi++;
        uint16_t bits = hud_bits[gi];
        for (int y = 0; y < 6; y++)
            for (int x = 0; x < 4; x++) {
                int on = (y < 5 && x < 3) ? (bits >> (14 - (y * 3 + x))) & 1 : 0;
                uint32_t o = FB_BASE + (uint32_t)(sy + fy + y * 2) * STRIDE + (sx + fx + x * 2);
                uint8_t c = on ? 255 : 254;
                FB8[o] = c; FB8[o + 1] = c; FB8[o + STRIDE] = c; FB8[o + STRIDE + 1] = c;
            }
    }
}

/* canvas mode: through the back buffer's line table (written before the HUD) */
/* HUD colours (byte pairs): 255 / 254, or in game levels 112 / 96 - BG colour 0 of palettes 7 / 6,
   which no BG pixel uses and the game's palettes never reach (its sprite palette 7 is CRAM 241-255) */
static uint32_t hud_on_c = 0xFFFF, hud_off_c = 0xFEFE;
static void hud_text_cv(int sx, int sy, const char *s)
{
    uint32_t la[12];
    const uint32_t on = hud_on_c, off = hud_off_c;
    for (int j = 0; j < 12; j++) la[j] = (uint32_t)FB16[sy + j] * 2 + sx;     /* even */
    for (; *s; s++) {
        int gi = 0;
        while (hud_chars[gi] && hud_chars[gi] != *s) gi++;
        uint32_t bits = hud_bits[gi];
        for (int y = 0; y < 6; y++, bits <<= 3) {          /* (constant shifts: no libgcc calls) */
            uint32_t b = y < 5 ? (bits >> 12) & 7 : 0;
            uint32_t w0 = (b & 4 ? on : off) << 16 | (b & 2 ? on : off);
            uint32_t w1 = (b & 1 ? on : off) << 16 | off;
            for (int j = y * 2; j < y * 2 + 2; j++) {
                uint32_t o = la[j];
                if (!(o & 2)) {
                    volatile uint32_t *d = (volatile uint32_t *)(0x24000000 + o);
                    d[0] = w0; d[1] = w1;
                } else {
                    volatile uint16_t *d = (volatile uint16_t *)(0x24000000 + o);
                    d[0] = w0 >> 16; d[1] = (uint16_t)w0; d[2] = w1 >> 16; d[3] = (uint16_t)w1;
                }
                la[j] = o + 8;
            }
        }
    }
}

/* ---- the in-level options menu (while the game is paused: $0579 bit 6) */
static int pm_open, pm_sel, pm_fg_reload;
static uint32_t pm_req;                   /* (I12) the button the menu sends the paused game (Start: resume,
                                             Select: exit) - once its unpause lock has run out */
static int start_block;                   /* (I12) Start still held from our level menu: not the game's */
static int pm_shown, pm_frz;              /* (I12) pictures with the menu as it is (pm_key); no new pictures */
static uint32_t pm_key;
#define PM_KEY() ((uint32_t)pm_sel | (uint32_t)fskip << 4 | (uint32_t)pipe_mode << 5 | (uint32_t)fg_on << 6 | (uint32_t)rip_on << 7 | (uint32_t)obj_wide << 8 | (uint32_t)pm_stats << 9)
/* 60 FPS mode (menu option, off by default: it adds a frame of input lag): the slave draws the whole
   picture of game frame N (BG strips, sprites, line table, HUD: job 5 with every row) while the master
   runs the logic of frame N + 1; the picture is shown once that logic is done.  Simple canvas levels
   only (the fused job's levels with the chunky sprite tiles); the others keep the normal path. */
#ifndef PIPE_DEFAULT
#define PIPE_DEFAULT 0
#endif
static int pipe_mode = PIPE_DEFAULT, pipe_pend;
/* (I7) 60 FPS mode only where it reaches more pictures: per 64 pictures the vblanks they took; the
   slave alone could not draw Jungle Hijinxs in a frame (30 pictures a second, the normal mode 60) */
static int pipe_off, pipe_now;
static int pipe_async;                    /* (I17) the last 60 FPS mode picture's swap may be asynchronous */
static uint32_t pv_last, pv_sum, pv_n;
static void pipe_rate(void);
static int32_t pipe_cx, pipe_cy;
static uint32_t pipe_ticks;               /* this iteration's wait + flip (not logic) */
static uint32_t pipe_rest;                /* ... of which helping + waiting for the slave (no flip) */
static uint16_t pipe_t1;                  /* (FRT, vcount) when that flip was done */
static uint32_t pipe_v1, pipe_logic_last;
static int32_t pipe_gx, pipe_gy, pipe_bi;  /* the slave's picture: canvas origin, buffer */
static uint32_t pm_exit_addr;             /* exit to map: the level's "cleared" byte we set, restored later */
static uint8_t pm_exit_orig;
static int32_t pm_exit_wait;
/* (I21) the menu's items (drawn in the small font, 10 lines apart) */
enum { PM_RESUME, PM_FSKIP, PM_PIPE, PM_FG, PM_RIPPLE, PM_OBJ, PM_STATS, PM_EXIT, PM_LEVELS, PM_N };
static int pm_stats;                      /* (I21c) STATS: frame rate, SH-2 master / slave %, 68000 % (main's show_hud) */
/* (I21) OBJECT RANGE: WIDE (default) = the game's sprite spawn / keep-alive windows follow the 32X's
   wider view (+32 px past each side, as the SNES's own windows reach past its 256 px view): objects no
   longer vanish / pop in at the picture's edges.  SNES = the original windows (game logic exactly as
   the SNES's; objects can vanish just inside the 32X picture).  r65_ws (rompatch WS_TUNE). */
#ifndef OBJ_WIDE_DEFAULT
#define OBJ_WIDE_DEFAULT 1
#endif
static int obj_wide = OBJ_WIDE_DEFAULT;
/* (I21) 5x7 menu font, characters 32..95, one byte per row (bit 4 = left column) */
static const uint8_t font57[64][7] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00},   /* ' ' */
    {0x04,0x04,0x04,0x04,0x04,0x00,0x04},   /* '!' */
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04},   /* '"' */
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04},   /* '#' */
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04},   /* '$' */
    {0x18,0x19,0x02,0x04,0x08,0x13,0x03},   /* '%' */
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04},   /* '&' */
    {0x04,0x04,0x08,0x00,0x00,0x00,0x00},   /* "'" */
    {0x02,0x04,0x08,0x08,0x08,0x04,0x02},   /* '(' */
    {0x08,0x04,0x02,0x02,0x02,0x04,0x08},   /* ')' */
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04},   /* '*' */
    {0x00,0x04,0x04,0x1F,0x04,0x04,0x00},   /* '+' */
    {0x00,0x00,0x00,0x00,0x0C,0x04,0x08},   /* ',' */
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},   /* '-' */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C},   /* '.' */
    {0x00,0x01,0x02,0x04,0x08,0x10,0x00},   /* '/' */
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},   /* '0' */
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},   /* '1' */
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},   /* '2' */
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},   /* '3' */
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},   /* '4' */
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},   /* '5' */
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},   /* '6' */
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},   /* '7' */
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},   /* '8' */
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},   /* '9' */
    {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00},   /* ':' */
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04},   /* ';' */
    {0x02,0x04,0x08,0x10,0x08,0x04,0x02},   /* '<' */
    {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00},   /* '=' */
    {0x08,0x04,0x02,0x01,0x02,0x04,0x08},   /* '>' */
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04},   /* '?' */
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04},   /* '@' */
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},   /* 'A' */
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},   /* 'B' */
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},   /* 'C' */
    {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},   /* 'D' */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},   /* 'E' */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},   /* 'F' */
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},   /* 'G' */
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},   /* 'H' */
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},   /* 'I' */
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},   /* 'J' */
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11},   /* 'K' */
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},   /* 'L' */
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},   /* 'M' */
    {0x11,0x11,0x19,0x15,0x13,0x11,0x11},   /* 'N' */
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},   /* 'O' */
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},   /* 'P' */
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},   /* 'Q' */
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},   /* 'R' */
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},   /* 'S' */
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},   /* 'T' */
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},   /* 'U' */
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},   /* 'V' */
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0A},   /* 'W' */
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},   /* 'X' */
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},   /* 'Y' */
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},   /* 'Z' */
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04},   /* '[' */
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04},   /* '\\' */
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04},   /* ']' */
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04},   /* '^' */
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04},   /* '_' */
};
/* (I21) the pause menu in the small font: 6 px per character, 7 rows + the shadow */
static void text57_cv(int x, int y, const char *s, uint8_t on, uint8_t sh)
{
    for (; *s; s++, x += 6) {
        int c = *s - 32;
        if (c < 0 || c > 63) c = 31;
        const uint8_t *g = font57[c];
        for (int r = 0; r < 7; r++) {
            uint32_t bits = g[r];
            if (!bits) continue;
            uint32_t a0 = (uint32_t)FB16[y + r] * 2 + x, a1 = (uint32_t)FB16[y + r + 1] * 2 + x;
            for (int b = 0; b < 5; b++)
                if (bits & (0x10 >> b)) { FB8[a1 + b + 1] = sh; FB8[a0 + b] = on; }
        }
    }
}
static void box_cv(int x, int y, int w, int h, uint8_t c)
{
    uint32_t v = c * 0x01010101u;
    for (int j = 0; j < h; j++) {
        uint32_t a = (uint32_t)FB16[y + j] * 2 + x;
        int i = 0;
        for (; i < w && ((a + i) & 3); i++) FB8[a + i] = c;
        for (; i + 4 <= w; i += 4) *(volatile uint32_t *)((uint32_t)FB8 + a + i) = v;
        for (; i < w; i++) FB8[a + i] = c;
    }
}

/* same font for comp levels: word writes, each line moved by its band's shift P (even) so the text
   stays put on screen */
static void hud_text_fast(int x, int y, const char *s)
{
    for (; *s; s++, x += 8) {
        int gi = 0;
        while (hud_chars[gi] && hud_chars[gi] != *s) gi++;
        uint16_t bits = hud_bits[gi];
        for (int r = 0; r < 6; r++) {
            uint16_t w[4] = { 0xFEFE, 0xFEFE, 0xFEFE, 0xFEFE };
            if (r < 5) {
                uint32_t b = bits >> (12 - r * 3);
                if (b & 4) w[0] = 0xFFFF;
                if (b & 2) w[1] = 0xFFFF;
                if (b & 1) w[2] = 0xFFFF;
            }
            for (int k = 0; k < 2; k++) {
                int yy = y + r * 2 + k;
                volatile uint16_t *d = FB16 + (FB_BASE + (uint32_t)yy * STRIDE + x + (comp_wave[yy >> 2][0] & 3)) / 2;
                d[0] = w[0]; d[1] = w[1]; d[2] = w[2]; d[3] = w[3];
            }
        }
    }
}

static void fmt_num(char *p, int v, int digits)
{
    for (int i = digits - 1; i >= 0; i--) { p[i] = '0' + (v % 10); v /= 10; }
}

#ifdef BENCH
static void wait_flip(void);
static void write_line_table(int fx, int fy);
/* memory timing benchmark (MiSTer vs emulators): cycles per access = FRT ticks * 8 / n */
#define BN 2048
SDRAM_CODE __attribute__((noinline)) static uint32_t b_rd(uint32_t a, uint32_t st, uint32_t n)
{
    uint32_t s = 0;
    do { s += *(volatile uint32_t *)a; a += st; } while (--n);
    return s;
}
SDRAM_CODE __attribute__((noinline)) static uint32_t b_wr(uint32_t a, uint32_t st, uint32_t n)
{
    do { *(volatile uint32_t *)a = n; a += st; } while (--n);
    return 0;
}
SDRAM_CODE __attribute__((noinline)) static uint32_t b_ww(uint32_t a, uint32_t st, uint32_t n)
{
    do { *(volatile uint16_t *)a = n; a += st; } while (--n);
    return 0;
}
/* write a long (uncached) then read the next 16-byte line (cached): read after write in one 32-byte block */
SDRAM_CODE __attribute__((noinline)) static uint32_t b_raw(uint32_t a, uint32_t st, uint32_t n)
{
    uint32_t s = 0;
    do { *(volatile uint32_t *)(a | 0x20000000) = n; s += *(volatile uint32_t *)(a + 16); a += st; } while (--n);
    return s;
}
typedef struct { const char *name; uint32_t (*fn)(uint32_t, uint32_t, uint32_t); uint32_t a, st, slave; } bench_t;
static volatile uint32_t bench_slave;       /* 1: slave streams DDR line fills, 2: ROM line fills */
static void bench(void)
{
    static const bench_t T[] = {
        { "loop+hit", b_rd, 0x06000400, 0 },
        { "DDR fill s16", b_rd, 0x0601C000, 16 },
        { "DDR fill s32", b_rd, 0x0601C000, 32 },
        { "BRAM fill s16", b_rd, 0x06000000, 16 },
        { "ROM fill s16", b_rd, 0x02200000, 16 },
        { "DDR unc rd", b_rd, 0x2601C000, 4 },
        { "BRAM unc rd", b_rd, 0x26000000, 4 },
        { "ROM unc rd", b_rd, 0x22200000, 4 },
        { "DDR wr same", b_wr, 0x2601C000, 0 },
        { "BRAM wr same", b_wr, 0x26007740, 0 },
        { "DDR wr seq", b_wr, 0x2601C000, 4 },
        { "DDR wr+rd", b_raw, 0x0602C000, 32 },
        { "FB wr long", b_wr, 0x24000200, 4 },
        { "FB ovr long", b_wr, 0x24020200, 4 },
        { "FB wr word", b_ww, 0x24000200, 2 },
        { "COMM rd", b_rd, 0x20004020, 0 },
        { "DDR fill +S ddr", b_rd, 0x0601C000, 16, 1 },
        { "BRAM fill +S ddr", b_rd, 0x06000000, 16, 1 },
        { "ROM fill +S ddr", b_rd, 0x02200000, 16, 1 },
        { "DDR fill +S rom", b_rd, 0x0601C000, 16, 2 },
    };
    static int res[2][sizeof(T) / sizeof(T[0])];
    REG8(SYS_INTMASK) = 0x08;                            /* V-int only: vcount */
    for (int pass = 0; pass < 2; pass++)
        for (unsigned i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
            REG8(0xFFFFFE92) = REG8(0xFFFFFE92) | 0x10;      /* purge the cache */
            *UNCACHED(&bench_slave) = T[i].slave;
            uint32_t v = vcount, calls = 0;
            while (vcount == v)
                ;
            v = vcount;
            /* calls of BN accesses in 20 frames (383523 cycles each) */
            while (vcount - v < 20) {
                T[i].fn(T[i].a, T[i].st, BN);
                calls++;
            }
            res[pass][i] = (int)(76704600u / (calls * BN));
            *UNCACHED(&bench_slave) = 0;
        }
    CRAM[0] = 0x0000; CRAM[254] = 0x8000; CRAM[255] = 0xFFFF; CRAM[253] = 0x83E0;
    for (int b = 0; b < 2; b++) {
        for (int i = 0x100; i < 0x10000; i++) FB16[i] = 0;
        text(24, 6, "SH-2 cycles x10 per access", 253);
        for (unsigned i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
            char l[16];
            text(24, 20 + i * 10, T[i].name, 255);
            fmt_num(l, res[0][i], 5); l[5] = ' '; fmt_num(l + 6, res[1][i], 5); l[11] = 0;
            text(150, 20 + i * 10, l, 255);
        }
        write_line_table(0, 0);
        wait_flip();
    }
    for (;;)
        ;
}
#endif

/* ------------------------------------------------------------ flip / line table */
static int cur_fb;
static int32_t comp_P;               /* comp levels: whole-line pixel shift of the back buffer */
static const uint16_t *pending_pal;
static int pending_sky;                  /* (I13) pending_pal's colour 0 is the sky too (menu / world images) */

/* (I17) FLIP_ASYNC: present() can ask for the swap during the display and return - the swap happens
   at the next vblank while the master runs the next logic frame (it waited for that vblank before:
   ~half a frame per picture, 20-30 % of the master in the levels that miss 60).  cur_fb is the new
   front buffer from the request on; nothing may draw into the back buffer until the swap is seen:
   fb_sync() before the master's first drawing after the logic (and in wait_flip / present / load
   paths), the slave's early job waits for it itself. */
static int af_pend, af_last;             /* (af_last: the last picture was a gameplay one, FLIP_ASYNC on) */
static uint32_t af_n, af_wait;            /* (test counters: async pictures, FRT ticks waited in fb_sync) */
static void fb_sync(void)
{
    if (!af_pend) return;
    while ((REG16(VDP_FBCTL) & 1) != cur_fb)
        ;
    af_pend = 0;
}

static void wait_flip(void)
{
    fb_sync();
    /* drain the VDP's frame buffer write FIFO first: a read waits until it is empty (MiSTer
       VDP.sv); queued writes would otherwise land in the buffer that is about to be shown */
    (void)*(volatile uint16_t *)0x24000000;
    REG16(VDP_FBCTL) = cur_fb ^ 1;
    while ((REG16(VDP_FBCTL) & 1) == cur_fb)
        ;
    cur_fb ^= 1;
}

static void write_line_table(int fx, int fy)
{
    uint16_t w = (FB_BASE >> 1) + (uint16_t)(fy * (STRIDE / 2)) + (fx >> 1);
    for (int y = 0; y < 256; y++) {
        FB16[y] = w;
        if (y < SCREEN_H - 1) w += STRIDE / 2;
    }
}

static void hexs(char *d, uint32_t v, int n)
{
    static const char hx[] = "0123456789ABCDEF";
    for (int i = n - 1; i >= 0; i--, v >>= 4) d[i] = hx[v & 15];
    d[n] = 0;
}
/* (I1) stack guards: 16 bytes at the bottom of the master stack (0603E800 since I13, was 0603F000, above the RAM the linker
   fills: mars.ld) and between the two stacks (0603F7F0, the slave's below 06040000).  A broken one stops
   the game with the STUCK page (STK) instead of corrupting memory quietly. */
#define CANARY 0xC0FFEE11u
static void canary_set(void)
{
    for (int i = 0; i < 4; i++) { ((volatile uint32_t *)0x2603E800)[i] = CANARY; ((volatile uint32_t *)0x2603F7F0)[i] = CANARY; }
}
static uint32_t canary_bad(void)
{
    uint32_t b = 0;
    for (int i = 0; i < 4; i++) {
        if (((volatile uint32_t *)0x2603E800)[i] != CANARY) b |= 1;
        if (((volatile uint32_t *)0x2603F7F0)[i] != CANARY) b |= 2;
    }
    return b;
}
static void hang_page(void)
{
    extern volatile uint32_t r65_dbg_key, r65_dbg_pc;
    extern uint32_t game_wai_pc(void);
    char b[12];
    write_line_table(0, 0);
    box(0, 0, SCREEN_W, SCREEN_H, 254);
    text(24, 20, "STUCK - PLEASE TAKE A PHOTO", 255);
    text(24, 50, "M STAGE", 255); hexs(b, dbg_stage, 4); text(150, 50, b, 255);
    text(24, 66, "S JOB", 255); hexs(b, *UNCACHED(&s_job_now), 4); text(150, 66, b, 255);
    text(24, 82, "KEY", 255); hexs(b, r65_dbg_key, 8); text(150, 82, b, 255);
    text(24, 98, "IPC", 255); hexs(b, r65_dbg_pc, 6); text(150, 98, b, 255);
    text(24, 114, "WAI", 255); hexs(b, game_wai_pc(), 6); text(150, 114, b, 255);
    text(24, 130, "ENT", 255); hexs(b, game_wram16(0x3E), 4); text(150, 130, b, 255);
    text(24, 146, "FRAME", 255); hexs(b, game_stats.frames, 6); text(150, 146, b, 255);
    text(24, 162, "ERR", 255); hexs(b, game_stats.err, 8); text(150, 162, b, 255);
    text(24, 178, "M PC", 255); hexs(b, m_irq_pc, 8); text(150, 178, b, 255);
    text(24, 194, "S PC", 255); hexs(b, *UNCACHED(&s_irq_pc), 8); text(150, 194, b, 255);
    text(24, 210, "STK", 255); hexs(b, canary_bad(), 2); text(150, 210, b, 255);
    /* (I18b) the 68000's side, refreshed every half second (hang_show): its command register, the
       SRAM handshake words, and its vblank counter (the pad sample number: still counting = its
       interrupts run) */
    text(232, 50, "CMD", 255); hexs(b, COMM(C_CMD), 4); text(272, 50, b, 255);
    text(232, 66, "C2", 255); hexs(b, COMM(2), 4); text(272, 66, b, 255);
    text(232, 82, "C4", 255); hexs(b, COMM(4), 4); text(272, 82, b, 255);
    text(232, 98, "VBL", 255); hexs(b, COMM(C_PAD) >> 8, 2); text(272, 98, b, 255);
}
static void hang_show(void)
{
    hang_armed = 0;
    REG16(VDP_DISPMODE) = 0x0001;           /* (a hang while loading: the 32X layer is off) */
    CRAM[254] = 0x8000; CRAM[255] = 0xFFFF;
    for (int i = 0; i < SCREEN_H; i++) *UNCACHED(&sky_ram[i]) = 0x8000;
    CRAM[0] = 0x8000;
    sft_show = 0;
    for (int k = 0; k < 2; k++) {
        hang_page();
        REG16(VDP_FBCTL) = cur_fb ^ 1;          /* show it (at the next vblank) */
        while ((REG16(VDP_FBCTL) & 1) == cur_fb)
            ;
        cur_fb ^= 1;
    }
    for (;;) {                                  /* (I18b) keep the 68000's values live */
        for (int v = 0; v < 30; v++) {
            while (REG16(VDP_FBCTL) & VDP_VBLK)
                ;
            while (!(REG16(VDP_FBCTL) & VDP_VBLK))
                ;
        }
        hang_page();
        REG16(VDP_FBCTL) = cur_fb ^ 1;
        while ((REG16(VDP_FBCTL) & 1) == cur_fb)
            ;
        cur_fb ^= 1;
    }
}


static void write_line_table_ripple(int fx, int fy, uint8_t *sft)
{
    for (int y = 0; y < 256; y++) {
        int yy = y < SCREEN_H ? y : SCREEN_H - 1;
        int d = ripple_pat[(yy - ripple_phase) & 63];
        int x = fx + d;
        FB16[y] = (FB_BASE >> 1) + (uint16_t)((yy + fy + d) * (STRIDE / 2)) + (x >> 1);
        if (y < SCREEN_H) sft[y] = x & 1;
    }
}

/* canvas mode: screen line y shows canvas line (gy + y) mod CV_H from grid x gx (+ the ripple) */
/* canvas mode 3: the fog bands (comp_wave, 4 lines each) - window over all of them, line table, and
   the tiles under the HUD (lines 4-31, x 4-160).  Fog x is taken relative to cv_xbase (a multiple of
   the layer's 256-pixel period). */
static int32_t cv_xbase;
#define CV3_MAXR 32                      /* (Misty Mine: sprite rectangles kept per buffer) */
/* (in map_mem's last 1 KB: SDRAM is full - the fog levels do not use it; other screens leave their data
   there (the map's sprite tiles): zeroed when the fog level is set up - cv3_reset) */
#define CV3_DIRTY ((int16_t (*)[CV3_MAXR][4])((uint8_t *)map_mem + sizeof map_mem - 1024))
#define CV3_SR    ((int16_t (*)[4])((uint8_t *)map_mem + sizeof map_mem - 1024 + 2 * CV3_MAXR * 8))
#define CV3_ND    ((int32_t *)((uint8_t *)map_mem + sizeof map_mem - 1024 + 3 * CV3_MAXR * 8))
/* Slipslide glimmer (the game's routine around $1DF5-$1DFD): a random wait, then 4 glimmers of 8
   frames, each at a new random position, fading in and out (BG3 palette 0-3 grey ramps) */
static int32_t glow_t, glow_base, glow_x, glow_y;
static uint32_t glow_rng = 0x1234567;
static volatile uint16_t glow_col = 0xFFFF;  /* CRAM[252] at the next present (0xFFFF: unchanged) */
/* glimmer colours: CRAM 240 + palette * 3 + shade - 1 = scene + 2 x glow (50% checkerboard flipped per
   frame -> the eye sees scene + glow), recomputed per glimmer step */
static uint16_t glow_cols[12], glow_mean;
static const uint16_t *glow_peak;
static volatile int glow_dirty;
static uint32_t glow_rand(void)
{
    glow_rng = glow_rng * 1103515245u + 12345u;
    return glow_rng >> 16;
}
static int glow_frame(shared_t *sh)
{
    /* dot grey by t & 7 (the SNES peak is at t & 7 = 4); the dimmest steps are skipped - dots darker
       than the scene would read as shadow, not light */
    static const uint8_t scale[8] = { 0, 8, 18, 27, 31, 27, 18, 8 };
    if (--glow_t < 0) {
        glow_t = 0x60 + (glow_rand() & 0x3F);
        glow_base = (int32_t)(glow_rand() & 0x60) - 0x40;
    }
    if (glow_t & ~0x1F)
        return 0;
    if ((glow_t & 7) == 0) {
        glow_x = (int32_t)(glow_rand() & 0x3F) + glow_base;
        glow_y = ((((glow_x - glow_base) ^ 0xFF) & 0xFF) >> 1) - 0x50;
        return 0;
    }
    uint32_t g = scale[glow_t & 7];
    if (!g)
        return 0;
    if (glow_peak) {
        for (int i = 0; i < 12; i++) {
            uint32_t a = (glow_peak[i] * g * 2) >> 5, c = 0x8000;
            for (int j = 0; j < 15; j += 5) {
                uint32_t v = ((glow_mean >> j) & 31) + a;
                c |= (v > 31 ? 31 : v) << j;
            }
            glow_cols[i] = (uint16_t)c;
        }
        glow_dirty = 1;
    } else
        glow_col = 0x8000 | g | g << 5 | g << 10;
    sh->ovx[0] = sh->ovx[1] = glow_x & 0xFF;
    sh->ovy[0] = sh->ovy[1] = glow_y & 0xFF;
    return 1;
}

/* hanging lamps (walkway levels): image origin on screen x = lamp x - camx - 91, y = 481 - lamp y - camy
   (the object's y counts up from 504; the capture had the apex 24 lines below the origin) */
static uint16_t lamp_pal[12];
static const uint16_t *lamp_scene;     /* mean unlit scene colour under cone levels 1-10 */
static const uint16_t *lamp_w;         /* light colour per channel, 1/16 units */
static uint16_t lamp_cols[10];
static uint32_t lamp_fc, lamp_d = 99;
static volatile int lamp_dirty;
static int lamp_frame(shared_t *sh, int32_t camx, int32_t camy)
{
    /* lamp flicker (CODE_80B800): d = 0-3 from two triangle waves of the frame counter; the cone dots
       are scene + 2 x (level - d) (50% checkerboard flipped per frame) */
    uint32_t f = ++lamp_fc, e = f >> 2;
    uint32_t t1 = e & 0x7F, t2 = ((e + f) >> 1) & 0x7F;
    if (t1 & 0x40) t1 ^= 0x7F;
    if (t2 & 0x40) t2 ^= 0x7F;
    uint32_t d = (t1 + t2) >> 5;
    if ((f & 1) && ((t1 + t2) & 16)) d++;
    if (d != lamp_d && lamp_scene) {
        lamp_d = d;
        for (int k = 0; k < 10; k++) {
            int32_t l = (k + 1) - (int32_t)d;
            uint32_t c = lamp_scene[k], o = 0x8000;
            if (l <= 0) {                           /* no light: no priority = the Genesis scene shows */
                lamp_cols[k] = 0;
                continue;
            }
            for (int j = 0; j < 3; j++) {
                uint32_t v = ((c >> (5 * j)) & 31) + ((2 * l * lamp_w[j]) >> 4);
                o |= (v > 31 ? 31 : v) << (5 * j);
            }
            lamp_cols[k] = (uint16_t)o;
        }
        lamp_dirty = 1;
    }
    (void)sh; (void)camx; (void)camy;
    return 0;
}

/* Ropey Rampage rain (BG3 over BG1): pixels drawn into the back buffer each frame, the overwritten
   bytes saved (addr | byte << 24) and put back before that buffer is drawn again.  The save lists
   use map_mem, which levels without overlays leave free (the cv_ovd marks sit at 16 KB). */
static int cv_x0par;
static uint32_t rain_n, rain_f;
#ifdef RAINCHECK
static uint32_t rc_bad, rc_y;
#endif
static const uint8_t *rain_pix;
/* weather triggers (sprite-list type-8 entries): rain off -> the rain's top edge sweeps down 8 px a
   frame; darkness off -> 32-step linear fade (one step per 8 frames) to the day palette, sky and
   Genesis CRAM (the 68000 lerps its CRAM from COMM10 = 0x5A00 | step) */
static uint32_t rain_state, rain_cut, wx_rain_x, wx_dark_x, wx_k, wx_t, wx_on, wx_first;
static const uint16_t *wx_palN, *wx_palD, *wx_skyN, *wx_skyD;
static uint16_t wx_pal[128];
static volatile int wx_dirty;
/* game mode (Ropey Rampage): the 32X layer's colours 1-127 follow the game's CGRAM writes (its own
   darkness fade and the lightning), the sky its HDMA table at $1B41; the Genesis planes keep the
   keyframes (key 2 = lightning, while the game's sky table is the flash one, DATA_B9946A) */
static int bg_mirror;
static int hud_early = 1;
#ifdef PROFT
static uint32_t proft_pct[7];
#endif
#ifdef PROFT
static uint16_t pt[8];
#ifdef PICPROF
static uint32_t ptv[8], pp_full, pp_n;
#define PT(i) (pt[i] = frt_read(), ptv[i] = vcount)
#else
#define PT(i) (pt[i] = frt_read())
#endif
#else
#define PT(i)
#endif          /* job 3 restores under the old HUD (adaptive) */
static uint32_t bg_flash_col, bg_sky_t;
static void bg_mirror_frame(void)
{
    extern uint8_t wram_lo[];
    if (spr_bgpal_dirty || !(bg_sky_t++ & 7)) {
        /* (decoded straight into sky_ram: the WRAM table path always fills every line; I11 - SDRAM) */
        game_hdma_sky(0x001B41, sky_next, SCREEN_H);
        sky_next_on = 1;
        wx_dirty = 2;                     /* (present: the mirrored colours) */
    }
}
static void wx_frame(int32_t camx)
{
    if (!wx_on) return;
    if (rain_state == 0 && camx + 320 >= (int32_t)wx_rain_x) { rain_state = 1; rain_cut = 0; }
    if (wx_t == 0 && camx + 320 >= (int32_t)wx_dark_x) {
        wx_t = 1;
        /* (re)started past the darkness change (a checkpoint, back from a bonus room): the SNES
           palette is already the dark one - no fade */
        if (wx_first) { wx_k = 31; wx_t = 8; }
    }
    wx_first = 0;
    if (bg_mirror) {
        extern uint8_t wram_lo[];
        bg_mirror_frame();
        if (wx_t && wx_k < 32 && (wx_t++ & 7) == 0) wx_k++;
        uint32_t s0 = wram_lo[0x1B44] | wram_lo[0x1B45] << 8;
        COMM(10) = s0 == bg_flash_col ? 0x4000 | 2 << 11 | 2 << 8 | 32 : 0x4000 | 1 << 8 | wx_k;
        return;
    }
    if (wx_t && wx_k < 32 && (wx_t++ & 7) == 0) {
        wx_k++;
        uint32_t k = wx_k;
        for (int i = 1; i < 128; i++) {
            uint32_t n = wx_palN[i], d = wx_palD[i];
            if (!(i & 15)) continue;
            int32_t r = (n & 31) + ((((int32_t)(d & 31) - (int32_t)(n & 31)) * (int32_t)k) >> 5);
            int32_t g = ((n >> 5) & 31) + ((((int32_t)((d >> 5) & 31) - (int32_t)((n >> 5) & 31)) * (int32_t)k) >> 5);
            int32_t b = ((n >> 10) & 31) + ((((int32_t)((d >> 10) & 31) - (int32_t)((n >> 10) & 31)) * (int32_t)k) >> 5);
            wx_pal[i] = (uint16_t)(0x8000 | r | g << 5 | b << 10);
        }
        for (int i = 0; i < SCREEN_H; i++) {
            uint32_t n = wx_skyN[i], d = wx_skyD[i];
            int32_t r = (n & 31) + ((((int32_t)(d & 31) - (int32_t)(n & 31)) * (int32_t)k) >> 5);
            int32_t g = ((n >> 5) & 31) + ((((int32_t)((d >> 5) & 31) - (int32_t)((n >> 5) & 31)) * (int32_t)k) >> 5);
            int32_t b = ((n >> 10) & 31) + ((((int32_t)((d >> 10) & 31) - (int32_t)((n >> 10) & 31)) * (int32_t)k) >> 5);
            sky_next[i] = (uint16_t)(r | g << 5 | b << 10);
        }
        sky_next_on = 1;
        wx_dirty = 1;
    }
    COMM(10) = 0x4000 | 1 << 8 | wx_k;               /* keyframe 0 -> 1, step k of 32 */
}

/* snow levels (block kind 5): weather state 0-5 = the last trigger whose x the view has reached;
   palette keyframe fades of 8 steps x 8 frames (32X CRAM 1-127 + sky here, Genesis CRAM on the
   68000: COMM10 = 0x4000 | from << 11 | to << 8 | step).  State 5 = blizzard: the heavy snow overlay
   with full redraws (cv 0) instead of the canvas. */
static uint32_t ws_on, ws_cur, ws_from, ws_to, ws_k, ws_t, ws_ntrig, ws_shift, ws_start;
static const uint16_t *ws_trig, *ws_pkeys, *ws_skeys;
/* light-switch levels (block kind 6, Stop & Go Station / Loopy Lights): the barrels switch the
   lights when the view centre passes them (DK would touch them there) for their duration; Loopy's
   lights flicker out.  The state is the target of the fade engine above (1-step = instant). */
static uint32_t lt_on, lt_n, lt_flicker, lt_state, lt_timer, lt_flick;
static int32_t lt_prevc;
static const uint16_t *lt_bar;
static int32_t bm_ax0, bm_ay0;
static int game_on;
static const uint16_t *ms_blk;      /* temple: the millstone block (kind 7), or 0 */
static const uint16_t *ms_cells0;   /* its 4 tilemaps (SDRAM copy) */
static uint32_t lt_game;   /* Stop & Go / Loopy: the game's own lights state ($0579 bit $200, CODE_80B206 /
                             80B38B); the barrel-crossing estimate below is only for Torchlight now */
/* (I10) Torchlight in the game: the sprite palettes own CRAM 128-252, so there is no room for a lit
   copy of the BG colours.  CRAM 1-127 = the lit colours; a dark pixel (outside the beam) is drawn as
   bm_dmap[c]: the dark colours (c - 15: mostly black, some reds) merged into the spare entries (BG
   colour 0 of palettes 1-5, sprite colour 0 of palettes 0-7: never a pixel; 96 / 112 = HUD text) */
static const uint8_t bm_slots[] = { 16, 32, 48, 64, 80, 128, 144, 160, 176, 192, 208, 224, 240 };
#define BM_NS ((int)sizeof bm_slots)
static int bm_cram;                   /* present(): CRAM 1-127 + the slots again */
__attribute__((noinline)) static void bm_game_setup(const uint16_t *lit, const uint16_t *dark)
{
    /* unique dark colours (count, sum r g b), then the closest pairs merged until they fit */
    int32_t *cn = BM_TMP, *cr = cn + 128, *cg = cr + 128, *cb = cg + 128;
    uint8_t *cl = (uint8_t *)(cb + 128);   /* colour index -> cluster */
    int32_t n = 0;
    for (int i = 0; i < 128; i++) {
        cl[i] = 0;
        if (!(i & 15)) continue;
        uint32_t d = dark[i] & 0x7FFF, r = d & 31, g = (d >> 5) & 31, b = d >> 10;
        int k;
        for (k = 0; k < n; k++)
            if (cr[k] == (int32_t)r * cn[k] && cg[k] == (int32_t)g * cn[k] && cb[k] == (int32_t)b * cn[k]) break;
        if (k == n) { cn[n] = 0; cr[n] = cg[n] = cb[n] = 0; n++; }
        cn[k]++; cr[k] += r; cg[k] += g; cb[k] += b;
        cl[i] = (uint8_t)k;
    }
    while (n > BM_NS) {
        int32_t best = 0x7FFFFFFF, ba = 0, bb = 1;
        for (int a = 0; a < n; a++)
            for (int b = a + 1; b < n; b++) {
                int32_t dr = cr[a] / cn[a] - cr[b] / cn[b], dg = cg[a] / cn[a] - cg[b] / cn[b], db = cb[a] / cn[a] - cb[b] / cn[b];
                int32_t e = (dr * dr * 2 + dg * dg * 3 + db * db) * (cn[a] * cn[b] / (cn[a] + cn[b]) + 1);
                if (e < best) { best = e; ba = a; bb = b; }
            }
        cn[ba] += cn[bb]; cr[ba] += cr[bb]; cg[ba] += cg[bb]; cb[ba] += cb[bb];
        n--;
        for (int i = 0; i < 128; i++) if (cl[i] == bb) cl[i] = (uint8_t)ba; else if (cl[i] == n) cl[i] = (uint8_t)bb;
        cn[bb] = cn[n]; cr[bb] = cr[n]; cg[bb] = cg[n]; cb[bb] = cb[n];
    }
    for (int k = 0; k < BM_NS; k++)
        bm_slotcol[k] = k < n ? (uint16_t)(0x8000 | (cr[k] + cn[k] / 2) / cn[k] | ((cg[k] + cn[k] / 2) / cn[k]) << 5 |
                                            ((cb[k] + cn[k] / 2) / cn[k]) << 10) : 0x8000;
    for (int i = 0; i < 128; i++) bm_dmap[i] = (i & 15) ? bm_slots[cl[i]] : 0;
    for (int i = 0; i < 128; i++) wx_pal[i] = (i & 15) ? lit[i] : 0;
    for (int y = 0; y < 2 * SCREEN_H; y++) BM_LN[y] = bm_shown[y] = 0x7FFF;
    bm_cram = 1;
}
static void lt_frame(int32_t camx)
{
    if (lt_game && game_on) {
        extern uint8_t game_coldata;
        /* (I20) Blackout Basement (flicker field 2): the game's fixed colour, 30 = lights off */
        lt_state = lt_flicker == 2 ? game_coldata >= 16 : (game_wram16(0x579) >> 9) & 1;
        return;
    }
    int32_t c = camx + 160;
    for (uint32_t i = 0; i < lt_n && lt_prevc != (int32_t)0x80000000; i++) {
        int32_t x = lt_bar[i * 2];
        if ((lt_prevc < x && x <= c) || (c < x && x <= lt_prevc)) {
            lt_timer = lt_bar[i * 2 + 1];
            lt_state = 1;
            lt_flick = 0;
        }
    }
    lt_prevc = c;
    if (lt_timer && lt_timer != 0xFFFF && --lt_timer == 0) {   /* 0xFFFF: for good */
        if (lt_flicker) lt_flick = 24;
        else lt_state = 0;
    }
    if (lt_flick) {
        lt_flick--;
        lt_state = lt_flick ? (lt_flick >> 2) & 1 : 0;
    }
#ifdef BEAMON
    lt_state = 1;                                  /* (test builds: switched from the start) */
#endif
}
static void ws_heavy(shared_t *sh, int on)
{
    /* the blizzard is drawn by the Genesis (both planes, see md_vint); the 32X keeps its canvas and
       key 5's colours have no priority bit, so the Genesis flakes show in front of BG1 */
    (void)sh; (void)on;
}
static uint32_t ws_lerp(uint32_t n, uint32_t d, int32_t k, uint32_t sh)
{
    int32_t r = (n & 31) + ((((int32_t)(d & 31) - (int32_t)(n & 31)) * k) >> sh);
    int32_t g = ((n >> 5) & 31) + ((((int32_t)((d >> 5) & 31) - (int32_t)((n >> 5) & 31)) * k) >> sh);
    int32_t b = ((n >> 10) & 31) + ((((int32_t)((d >> 10) & 31) - (int32_t)((n >> 10) & 31)) * k) >> sh);
    return (uint32_t)(r | g << 5 | b << 10);
}
static void ws_frame(shared_t *sh, int32_t camx)
{
#ifdef NOWS
    return;
#endif
    if (!ws_on) return;
    if (lt_on) lt_frame(camx);
    if (ws_from == ws_to) {
        uint32_t tgt = ws_start;
        for (uint32_t i = 0; i < ws_ntrig; i++) {
            if (camx + 320 < (int32_t)ws_trig[i * 2]) break;
            tgt = ws_trig[i * 2 + 1];
        }
        if (lt_on) tgt = lt_state;
        if (tgt != ws_cur) {
            ws_from = ws_cur; ws_to = tgt; ws_k = 0; ws_t = ws_shift ? 0 : 7;
            if (tgt == 5) ws_heavy(sh, 1);
            else if (ws_cur == 5) ws_heavy(sh, 0);
        }
    }
    if (ws_from != ws_to && (++ws_t & 7) == 0) {
        ws_k++;
        const uint16_t *a = ws_pkeys + ws_from * 128, *b = ws_pkeys + ws_to * 128;
        uint16_t c = (uint16_t)ws_lerp(ws_skeys[ws_from], ws_skeys[ws_to], ws_k, ws_shift);
        for (int i = 0; i < SCREEN_H; i++) sky_next[i] = c;
        sky_next_on = 1;
        if (!sh->bm_game) {               /* (the game's beam level: CRAM is the lit + dark set, I10) */
            for (int i = 1; i < 128; i++)
                wx_pal[i] = (i & 15) ? (uint16_t)((b[i] & 0x8000) | ws_lerp(a[i], b[i], ws_k, ws_shift)) : 0;
            wx_dirty = 1;
        }
        if (ws_k >= (1u << ws_shift)) { ws_cur = ws_to; ws_from = ws_to; ws_k = 0; }
    }
    if (!sh->bm_lvl) COMM(10) = 0x4000 | ws_from << 11 | ws_to << 8 | ws_k;   /* (beam: the apex, main loop) */
}
/* 32X quirk (also in the MiSTer core, VDP.sv): a BYTE write of 0 to the framebuffer is ignored -
   only word writes store zero bytes.  So the rain saves and restores whole words.
   The rain is drawn by both CPUs at once (rain phase, after the canvas): the master does screen lines
   0-111, the slave 112-223, each with its own save lists (no word is shared between the halves).
   map_mem bytes: master lists 0 / 6800, master line table 13600; slave lists 17408 / 24208, slave
   line table 31008 (16 KB - 17 KB holds the overlay marks of other levels). */
#define RAIN_CAP 1700u
#define RAIN_IDX 245
/* the rain's 3 colours: CRAM 245-247 (= sprite palette 7 colours 5-7, which the game changes near
   Ropey's end: red rain), in game levels BG colour 0 of palettes 1-3 (never a BG pixel) */
static uint8_t rain_ci[3] = { 245, 246, 247 };
#define RAIN_LIST(half, b) ((uint32_t *)((uint8_t *)map_mem + ((half) ? 17408 : 0) + (b) * 6800))
#define RAIN_LTH(half)     ((uint32_t *)((uint8_t *)map_mem + ((half) ? 31008 : 13600)))
static uint32_t rain_cntA[2];
static void rain_restore_list(uint32_t *l, uint32_t n)
{
    for (uint32_t i = n; i-- > 0; ) {                 /* newest first: overlapping drops undo cleanly */
        uint32_t e = l[i];
        FB16[e & 0xFFFF] = (uint16_t)(e >> 16);
    }
}
static void rain_half(int half, const shared_t *p, shared_t *sh)
{
    int b = p->rbuf;
    uint32_t *l = RAIN_LIST(half, b), *lt = RAIN_LTH(half);
    uint32_t *cnt = half ? &sh->rcntB[b] : &rain_cntA[b];
    rain_restore_list(l, *cnt);
    *cnt = 0;
    if (!p->rdraw) return;
    int y0 = half ? SCREEN_H / 2 : 0, y1 = half ? SCREEN_H : SCREEN_H / 2;
    for (int y = y0; y < y1; y++) lt[y - y0] = (uint32_t)FB16[y] * 2 + p->rx0par;
    uint32_t H = p->rH, V = p->rV, cut = p->rcut, n = 0;
    const uint16_t *rows = (const uint16_t *)p->rpix, *px = rows + 257;
    for (int y = y0 < (int)cut ? (int)cut : y0; y < y1; y++) {   /* screen line y shows map row y + V + 1 */
        uint32_t r = (y + V + 1) & 255, a0 = lt[y - y0];
        for (uint32_t i = rows[r], i1 = rows[r + 1]; i < i1; i++) {
            uint32_t e = px[i], sx = (e - H) & 255, c = rain_ci[e >> 8];
            uint32_t a = a0 + sx;
            if (n < RAIN_CAP) { l[n++] = (a >> 1) | (uint32_t)FB16[a >> 1] << 16; FB8[a] = (uint8_t)c; }
            if (sx < 64 && n < RAIN_CAP) {
                a += 256;
                l[n++] = (a >> 1) | (uint32_t)FB16[a >> 1] << 16; FB8[a] = (uint8_t)c;
            }
        }
    }
    *cnt = n;
}
/* full redraw frames: restore both halves first (the redraw may not cover every old drop) */
static void rain_restore_all(int b, shared_t *sh)
{
    rain_restore_list(RAIN_LIST(0, b), rain_cntA[b]);
    rain_restore_list(UNCACHED(RAIN_LIST(1, b)), sh->rcntB[b]);
    rain_cntA[b] = 0;
    sh->rcntB[b] = 0;
}

static int cv_x0par = -1;              /* canvas mode without ripple: SFT of the last table */
static void cv3_rect(trect_t *r, int i0, int i1, int32_t xa, int32_t xb)
{
    int32_t x0 = 0x7FFFFFFF, x1 = -0x7FFFFFFF, y0 = 0x7FFFFFFF, y1 = -0x7FFFFFFF;
    for (int i = i0; i < i1; i++) {
        int32_t x = comp_wave[i][0] - cv_xbase, y = comp_wave[i][1] + 4 * i;
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
    r->x0 = (x0 + xa) >> 3;
    r->x1 = ((x1 + xb + 7) >> 3) + 1;
    r->y0 = y0 >> 3;
    r->y1 = ((y1 + 3) >> 3) + 1;
}
static void write_line_table_cv3(int flat)
{
    cv_x0par = (comp_wave[0][0] - cv_xbase + CV_XOFF) & 1;
    for (int y = 0; y < 256; y++) {
        int yy = y < SCREEN_H ? y : SCREEN_H - 1;
        int i = flat ? 0 : yy >> 2;      /* (I14) flat: the options menu is open - its text is drawn into
                                            this canvas, the bands' different rows cut / doubled its lines */
        /* (I14) no +1 row for odd (x + y) any more: it kept the checkerboard's screen phase but moved
           bands by a row whenever their parity changed - lines doubled / dropped at the band edges,
           flickering with the waves ("the mist effects double up") */
        uint32_t line = (uint32_t)(comp_wave[i][1] + yy) & (CV_H - 1);
        uint32_t x = (uint32_t)(comp_wave[i][0] - cv_xbase + CV_XOFF);
        FB16[y] = (uint16_t)((FB_BASE + line * CV_P + x) >> 1);
    }
}

static int dither_ripple;
static void write_line_table_cv(int32_t gx, int32_t gy, uint8_t *sft)
{
    /* dithered layers: shift by e (0/1 px) so the checkerboard stays put on screen while scrolling
       (each framebuffer then shows the opposite screen phase: a clean every-frame alternation) */
    int32_t e = dither_ripple ? ((gx + gy) & 1) : 0;
    gx += e;
    cv_x0par = (gx + CV_XOFF) & 1;
    int32_t cl = gy % CV_H;
    if (cl < 0) cl += CV_H;
    for (int y = 0; y < 256; y++) {
        int yy = y < SCREEN_H ? y : SCREEN_H - 1;
        int d = sft && !pm_open ? ripple_pat[(yy - ripple_phase) & 63] : 0;   /* (I18: flat under the options
                                                                                menu - its text is drawn
                                                                                through this table) */
        int32_t line = cl + yy + d;
        uint32_t x = (uint32_t)(gx + CV_XOFF + d);
        if (dither_ripple) { line -= d; x += d; }    /* dithered layer: (2, 0) keeps the checkerboard phase */
        if (line >= CV_H) line -= CV_H;
        FB16[y] = (uint16_t)((FB_BASE + (uint32_t)line * CV_P + x) >> 1);
        if (sft && y < SCREEN_H) sft[y] = x & 1;
    }
}

/* comp levels: per line shift P from the band's light x (the compositor aligned the light to longs);
   only bands whose P changed since this buffer's table was last written are rewritten */
static uint8_t comp_ltP[2][SCREEN_H / 4];
static void write_line_table_comp(uint8_t *sft, int buf)
{
    for (int i = 0; i < SCREEN_H / 4; i++) {
        int P = comp_wave[i][0] & 3;
        if (P == comp_ltP[buf][i]) continue;
        comp_ltP[buf][i] = P;
        int y1 = i == SCREEN_H / 4 - 1 ? 256 : i * 4 + 4;
        for (int y = i * 4; y < y1; y++) {
            int yy = y < SCREEN_H ? y : SCREEN_H - 1;
            FB16[y] = (FB_BASE >> 1) + (uint16_t)(yy * (STRIDE / 2)) + (P >> 1);
            if (y < SCREEN_H) sft[y] = P & 1;
        }
    }
}

/* ---- Misty Mine fog (CODE_80B4CA / CODE_80B547): BG2 fog scrolled per 4-line band by a wave that
   travels down one band per frame; grey level floor(v * I / 32) with I fading in 0 -> 31 */
static int comp_fog;
static const uint8_t *mw_sway, *mw_vrep2;
static const uint16_t *mw_qcol;
static uint32_t mw_S, mw_K, mw_I;
static int32_t mw_prev[SCREEN_H / 4][2];
static uint16_t mw_prevY, mw_prevXs;
static int mw_fresh;
static uint16_t comp_palbuf[256];
static volatile int comp_pal_dirty;

static int fog3;                       /* Misty performance variant: grey W x I / 31 at index 1 */
static void misty_palette(void)
{
    if (fog3) {
#ifdef FOGDEBUG
        return;
#endif
        /* checkerboard fog (as Poison Pond): index 1 + v shows grey 3 + 21 v I / 512 (the 50/50
           average about scene / 2 + 1.5 + 0.6 g, g = v I / 32 the SNES's added grey; I11: was 2 g + 3,
           at the densest mist nearly white dots - the level was hard to see) */
        for (uint32_t v = 1; v < 16; v++) {
            uint32_t g = 3 + ((v * mw_I * 21) >> 9);
            if (g > 31) g = 31;
            comp_palbuf[1 + v] = 0x8000 | g | g << 5 | g << 10;
        }
        comp_pal_dirty = 1;
        return;
    }
    for (uint32_t k = 0; k < mw_K; k++) {
        uint32_t g = ((uint32_t)mw_vrep2[k] * mw_I * 8) >> 9;
        for (uint32_t q = 0; q < mw_S; q++) {
            uint32_t c = mw_qcol[q];
            uint32_t r = (c & 31) + g, gg = ((c >> 5) & 31) + g, b = ((c >> 10) & 31) + g;
            if (r > 31) r = 31;
            if (gg > 31) gg = 31;
            if (b > 31) b = 31;
            comp_palbuf[k * mw_S + q] = r | gg << 5 | b << 10;
        }
    }
    for (uint32_t q = 0; q < mw_S; q++)        /* unlit bank (performance mode BG1) */
        comp_palbuf[mw_K * mw_S + q] = mw_qcol[q];
    for (uint32_t i = (mw_K + 1) * mw_S; i < 253; i++) comp_palbuf[i] = 0;
    comp_pal_dirty = 1;
}

static void misty_frame(uint32_t f, int32_t X, int32_t Y)
{
#ifdef MISTY_I0
    if (0) {
#else
    if ((f & 31) == 0 && mw_I < 31) {
#endif
        mw_I++;
        misty_palette();
    }
    uint32_t t = (f * 4) & 0x7F;
    if (t & 0x40) t ^= 0x7F;
    uint32_t a = mw_sway[t] + 0x80;
    if ((f * 4) & 0x80) a ^= 0xFF;
    t = (f * 2) & 0x7F;
    if (t & 0x40) t ^= 0x7F;
    uint32_t b = mw_sway[t] + 0x80;
    if ((f * 2) & 0x80) b ^= 0xFF;
    uint32_t v = (a + b) & 0xFFFF;
    v = ((v >> 1) + v) & 0xFFFF;
    v = (((v >> 6) + f) & 0xFFFF) >> 1;
    uint16_t xs = (uint16_t)(((uint16_t)X >> 2) + X);
    if (mw_fresh) {
        mw_fresh = 0;
        mw_prevY = Y;
        mw_prevXs = xs;
        for (int i = 0; i < SCREEN_H / 4; i++) { mw_prev[i][0] = xs + v; mw_prev[i][1] = Y + v; }
    }
    int32_t c4 = (f & 1) + (int16_t)(Y - mw_prevY);
    int32_t e4 = (f & 1) + (int16_t)(xs - mw_prevXs);
    mw_prevY = Y;
    mw_prevXs = xs;
    for (int i = SCREEN_H / 4 - 1; i > 0; i--) {
        comp_wave[i][0] = mw_prev[i][0] = mw_prev[i - 1][0] + e4;
        comp_wave[i][1] = mw_prev[i][1] = mw_prev[i - 1][1] + c4;
    }
    comp_wave[0][0] = mw_prev[0][0] = xs + v;
    comp_wave[0][1] = mw_prev[0][1] = Y + v;
    /* screen line y shows row V + y + 1.  Per-line SFT changes did not latch reliably (PicoDrive
       applies them 2 lines early), so the line table only moves bands by whole words.
       (I21c) Every band's column AND row offset rounded to an even value on its own (absolute): the
       checkerboard baked into the fog canvas keeps one phase on screen, and a band moves only when its
       own wave crosses a 2-pixel step.  Before, the bands took the TOP band's pixel parity (and a 1-px
       nudge for the checkerboard): the top band moves a pixel every other frame (the f & 1 drift), so
       every other band jumped a pixel and back with it - "sections of the mist switching position,
       then back, every other frame" (user) */
    for (int i = 0; i < SCREEN_H / 4; i++) {
        comp_wave[i][1] = (comp_wave[i][1] + 1) & ~1;
        comp_wave[i][0] &= ~1;
    }
#ifdef MISTY_FLAT
    for (int i = 1; i < SCREEN_H / 4; i++) { comp_wave[i][0] = comp_wave[0][0]; comp_wave[i][1] = comp_wave[0][1]; }
#endif
}

/* world maps: transparent map pixels show the SNES backdrop colour (palette colour 0) */
static void menu_sky(const uint16_t *pal)
{
    for (int i = 0; i < SCREEN_H; i++) sky_ram[i] = pal[0];
}

static void upload_palette(const uint16_t *pal)
{
    for (int i = 1; i < 128; i++) CRAM[i] = pal[i];
    for (int i = 128; i < 253; i++) CRAM[i] = 0x8000;
    CRAM[253] = 0x83FF | 0x8000;   /* yellow */
    CRAM[254] = 0x8000;            /* black  */
    CRAM[255] = 0xFFFF;            /* white  */
}

/* flip at the very start of the next vblank, then do vblank-only work */
static int disp_pending;                 /* the 32X layer goes back on at the next flip */
static int32_t pillar_l, pillar_r;       /* black bars (px) at the sides of a narrow room */
/* (I1) everything one screen leaves to the sprite renderer, back to neutral: called by every screen
   setup (level, world map, cabin, front-end screen, level menu) before it sets its own.  A level's
   ripple pattern drew the map's sprites through Coral Capers' wave bands (I4), its BG1 priority mask
   cut holes in the map Kongs (H11). */
static void render_state_reset(void)
{
    extern const uint16_t *volatile spr_prio_mask;
    extern const uint8_t *spr_rip_pat;
    extern int32_t spr_rip_phase, spr_xoff;
    extern volatile int32_t spr_ms_on;
    extern int spr_front_all;
    *UNCACHED(&spr_prio_mask) = 0;
    *UNCACHED(&spr_rip_pat) = 0;
    *UNCACHED(&spr_rip_phase) = 0;
    *UNCACHED(&spr_ms_on) = 0;
    *UNCACHED(&spr_xoff) = 0;
    *UNCACHED(&spr_front_all) = 0;
    pillar_l = pillar_r = 0;
    {   /* (I12) no beam outside its level: the map's sprites were shown with Torchlight's inverted
           palettes after leaving it (present: bm_game) */
        shared_t *ss = UNCACHED(&shared_state);
        ss->bm_lvl = ss->bm_on = ss->bm_game = 0;
        bm_cram = 0;
    }
#ifdef POISONMAP
    {   /* (test) what other screens may leave in map_mem's upper half: junk */
        volatile uint32_t *p = (volatile uint32_t *)UNCACHED((uint8_t *)map_mem + 16384);
        for (int i = 0; i < 4096; i++) p[i] = 0x7F3A7F3Au + i * 0x01000193u;
    }
#endif
    spr_ck_attach(0);                     /* (I3: the sprites' chunky tile copy lived on in map_mem, which
                                             Misty Mine / Poison Pond use for their fog map: garbage fog
                                             tiles; levels that leave map_mem free attach it again) */
}
/* the bars on the frame buffer about to be shown (after all its drawing) */
static void pillar_draw(void)
{
    if (!(pillar_l | pillar_r)) return;
    for (int y = 0; y < SCREEN_H; y++) {
        volatile uint8_t *l = FB8 + (uint32_t)FB16[y] * 2;
        for (int x = 0; x <= pillar_l; x++) l[x] = 96;      /* (CRAM 96: opaque black in game levels) */
        for (int x = 320 - pillar_r; x <= 320; x++) l[x] = 96;
    }
}
#ifdef PERFREC
static void perf_pic(void)
{
    /* window: game frames 120 .. 720 (10 s of game time); only the intervals between two pictures that
       both show gameplay (screen on, the game's main loop) count */
    static uint32_t on, lv, lf, prev_ok, pacc[24], pcnt[16], ls, lpb, lpx, laf, lafw, llost[4];
    volatile uint32_t *R = UNCACHED(&perf_rec[0]);
    uint32_t f = game_stats.frames, v = vcount, sid = *UNCACHED(&perf_sidle);
    int ok = game_on && !game_fblank && game_in_main_loop();
    if (!game_on || R[0] == 0x444F4E45) return;
    if (!on) {
        if (f < 120) return;
        on = 1; R[16] = FRAME_TICKS; R[17] = f;
    } else if (ok && prev_ok) {
        uint32_t d = v - lv;
        R[1]++; R[2] += d; R[3] += f - lf;
        R[4 + (d >= 8 ? 7 : d ? d - 1 : 0)]++;
        if (d > R[12]) R[12] = d;
        if (pipe_now) R[13]++;
        R[14] += sid - ls;
        for (int i = 0; i < 19; i++) R[19 + i] += perf_acc[i] - pacc[i];
        R[56] += perf_padb - lpb; R[57] += perf_padx - lpx;
        R[58] += af_n - laf; R[59] += af_wait - lafw;
        for (int i = 0; i < 4; i++) R[60 + i] += perf_lost[i] - llost[i];
    }
    if (on) for (int i = 0; i < 16; i++) R[40 + i] = perf_ln[i];
    prev_ok = ok; lv = v; lf = f; ls = sid; lpb = perf_padb; lpx = perf_padx; laf = af_n; lafw = af_wait; for (int i = 0; i < 4; i++) llost[i] = perf_lost[i];
    for (int i = 0; i < 24; i++) pacc[i] = perf_acc[i];
    for (int i = 0; i < 16; i++) pcnt[i] = perf_cnt[i];
    if (f - R[17] >= 600) { R[18] = 600; R[0] = 0x444F4E45; }
}
#endif
static uint32_t pv_avg[2], pv_win;       /* vblanks per 64 pictures, normal / 60 FPS mode (0: not known) */
static void pipe_rate(void)
{
    uint32_t d = vcount - pv_last;
    pv_last = vcount;
    if (!pipe_mode || !game_on || d > 8) { pv_sum = pv_n = 0; return; }
    pv_sum += d;
    if (++pv_n < 64) return;
    int m = pipe_now;
    pv_avg[m] = pv_sum;
    pv_sum = pv_n = 0;
    if (pv_avg[m] <= 66) return;                       /* (60 pictures a second: stay) */
    if (++pv_win >= 8) { pv_win = 0; pv_avg[!m] = 0; } /* (now and then: the other mode again) */
    if (!pv_avg[!m]) pipe_off = m;                     /* (the other mode not measured: try it) */
    else pipe_off = pv_avg[1] * 10 > pv_avg[0] * 9;    /* (I17: 60 FPS mode only where it is clearly faster -
                                                          >= 10 %: it costs a frame of input lag) */
}
volatile uint32_t dbg_presents __attribute__((used));   /* (test: pictures shown, read from SDRAM dumps) */
#ifdef PERFREC
static void perf_pic(void);
#endif
#ifndef FLIP_ASYNC
#define FLIP_ASYNC 1
#endif
static int flip_async = FLIP_ASYNC;
#ifdef SYNCWHY
volatile uint32_t sync_why[16] __attribute__((used));
#endif
/* (I21) C_FRAME: the picture counter, bits 9-15 the view offset spr_xoff (0..127) for the 68000's plane
   scroll (md update_scroll) - except in the lamp levels, whose cone flicker needs the game's whole $28 */
static int cf_view;                       /* (I21b) the Galleon boss (entrance 68) */
static uint16_t cframe(uint16_t f)
{
    return f;                             /* (I21c: no view offset for the 68000 any more - md update_scroll) */
}
static void present2(int32_t camx, int32_t snes_y, uint16_t *frame_no, int async)
{
    dbg_presents++;
#ifdef PERFREC
    perf_pic();
#endif
    pipe_rate();
#ifdef DEBUGLINE
    snes_y = current_line() + ((REG16(VDP_FBCTL) & VDP_VBLK) ? 1000 : 0);
#endif
    fb_sync();                            /* (I17: the last picture's swap first) */
    af_last = async && flip_async;        /* (a gameplay picture: the next logic catches up every vblank) */
#ifdef SYNCWHY
    {   /* (test) why a gameplay picture is synchronous: counts per reason (read from an SDRAM dump) */
        extern volatile uint32_t sync_why[16];
        if (async && flip_async) {
            sync_why[0]++;
            if (sky_next_on) sync_why[1]++;
            if (ripple) sync_why[2]++;
            if (sft_show) sync_why[3]++;
            if (disp_pending) sync_why[4]++;
            if (pending_pal) sync_why[5]++;
            if (spr_pal_dirty) sync_why[6]++;
            if (bm_cram) sync_why[7]++;
            if (comp_pal_dirty) sync_why[8]++;
            if (wx_dirty) sync_why[9]++;
            if (lamp_dirty) sync_why[10]++;
            if (glow_dirty || glow_col != 0xFFFF) sync_why[11]++;
            if (REG16(VDP_FBCTL) & VDP_VBLK) sync_why[12]++;
            else if (current_line() >= 208) sync_why[13]++;
        } else sync_why[14]++;
    }
#endif
    int af_ok = async && flip_async && !sky_next_on && (ripple ? !comp : !sft_show) && !disp_pending && !pending_pal && !spr_pal_dirty &&
                !bm_cram && !comp_pal_dirty && !wx_dirty && !lamp_dirty && !glow_dirty && glow_col == 0xFFFF;
    if (af_ok && ((REG16(VDP_FBCTL) & VDP_VBLK) || current_line() >= 222)) {
        /* (I19) a picture finished in the vblank (or just before it): the synchronous path waited for the
           NEXT vblank - a whole display period with the master idle (4 % of the master in JH).  The
           swap cannot happen in this vblank (the 68000 has already taken the camera for its planes),
           so: wait for the display to start and ask for the swap at the next vblank, as below */
        while (!(REG16(VDP_FBCTL) & VDP_VBLK))
            ;
        while (REG16(VDP_FBCTL) & VDP_VBLK)
            ;
    }
    if (af_ok && !(REG16(VDP_FBCTL) & VDP_VBLK) && current_line() < 208) {
        /* (I17) nothing for the swap's vblank but the shift register: ask for the swap now (it happens
           at the next vblank, with the camera posted for the 68000 as below) and go on */
        COMM(C_CAMX) = camx;
        COMM(C_CAMY) = snes_y;
        (*frame_no)++;
        if (lamp_n && game_on) {
            uint16_t f = (uint16_t)(game_wram16(0x28) - 1);
            if (f != (uint16_t)(*frame_no - 1)) *frame_no = f;
        }
        COMM(C_FRAME) = cframe(*frame_no);
        af_shift = comp ? (comp_P & 1) : cv_x0par >= 0 ? cv_x0par : (camx & 1);
        af_sft = ripple ? sft_tab[cur_fb ^ 1] : 0;  /* (the table rendered with the buffer shown next) */
        (void)*(volatile uint16_t *)0x24000000;     /* (the frame buffer write FIFO drained: wait_flip) */
        af_vint = 1;
        REG16(VDP_FBCTL) = cur_fb ^ 1;
        cur_fb ^= 1;
        af_pend = 1;
        af_n++;
        if (current_line() >= 222 || (REG16(VDP_FBCTL) & VDP_VBLK)) fb_sync();   /* (late after all: as wait_flip) */
        pending_sky = 0;
        dbg_stage = 70;
        return;
    }
    while (REG16(VDP_FBCTL) & VDP_VBLK)
        ;
    if (current_line() >= 222) {          /* too late to post the camera before vblank: skip */
        while (!(REG16(VDP_FBCTL) & VDP_VBLK))
            ;
        while (REG16(VDP_FBCTL) & VDP_VBLK)
            ;
    }
    COMM(C_CAMX) = camx;
    COMM(C_CAMY) = snes_y;
    (*frame_no)++;
    if (lamp_n && game_on) {              /* lamp levels: the 68K's swing / flicker follow the game's $28 */
        uint16_t f = (uint16_t)(game_wram16(0x28) - 1);
        if (f != (uint16_t)(*frame_no - 1)) *frame_no = f;
    }
    COMM(C_FRAME) = cframe(*frame_no);
    while (!(REG16(VDP_FBCTL) & VDP_VBLK))
        ;
    wait_flip();
    if (sky_next_on) {                    /* (I17) this picture's sky (the V interrupt set line 0 already) */
        for (int i = 0; i < SCREEN_H; i++) sky_ram[i] = sky_next[i];
        CRAM[0] = sky_ram[0];
        sky_next_on = 0;
    }
    if (disp_pending && --disp_pending == 0) REG16(VDP_DISPMODE) = 0x0001;   /* (I13: 2nd picture, as md_vint) */
    if (ripple && !comp) {
        sft_show = sft_tab[cur_fb];           /* table rendered with this buffer */
        REG16(VDP_SHIFT) = sft_tab[cur_fb][0];
    } else {
        sft_show = 0;
        REG16(VDP_SHIFT) = comp ? (comp_P & 1) : cv_x0par >= 0 ? cv_x0par : (camx & 1);
    }
    dbg_stage = 70;
    if (pending_pal) {                    /* (before the sprite palettes: it clears CRAM 128-252) */
        upload_palette(pending_pal);
        if (pending_sky) menu_sky(pending_pal);   /* (I13: with the palette, not before the picture) */
        pending_pal = 0;
    }
    pending_sky = 0;
    if (spr_pal_dirty) {
        uint32_t d = spr_pal_dirty;
        spr_pal_dirty = 0;
        int bm = UNCACHED(&shared_state)->bm_game;
        for (int p = 0; p < 8; p++)
            if (d >> p & 1)
                for (int i = p * 16 + 1; i < p * 16 + 16; i++) {
                    uint32_t c = spr_pal[i];
                    if (bm) {
                        /* (I10) Torchlight: the main screen is only BG3 (the beam) / the backdrop, the
                           scene with the sprites is the subscreen it subtracts - the game inverts every
                           palette; shown as in the beam: (30, 30, 31) - c */
                        int32_t r = 30 - (int32_t)(c & 31), g = 30 - (int32_t)((c >> 5) & 31), b = 31 - (int32_t)((c >> 10) & 31);
                        c = 0x8000 | (r < 0 ? 0 : r) | (g < 0 ? 0 : g) << 5 | (b < 0 ? 0 : b) << 10;
                    }
                    CRAM[128 + i] = (uint16_t)c;
                }
    }
    if (bm_cram) {                        /* (I10) the game's beam level: lit BG colours + the dark slots */
        bm_cram = 0;
        for (int i = 1; i < 128; i++) if (i & 15) CRAM[i] = wx_pal[i];
        for (int k = 0; k < BM_NS; k++) CRAM[bm_slots[k]] = bm_slotcol[k];
    }
    if (comp_pal_dirty) {
        /* (I3: in the game the sprite palettes are CRAM 128-252 - the fog levels' colours stay below:
           Misty Mine's fog brightening every 32 frames turned the Kongs into fog-coloured shapes) */
        int n = game_on && !comp ? 128 : 253;
        for (int i = 0; i < n; i++)
            if (!game_on || (i != 96 && i != 112)) CRAM[i] = comp_palbuf[i];   /* (96 / 112: menu + HUD text, I11) */
        comp_pal_dirty = 0;
    }
    if (wx_dirty == 2) {                  /* game mode: the game's BG colours (bg_mirror) */
        uint32_t d = spr_bgpal_dirty;
        spr_bgpal_dirty = 0;
        for (int p = 0; p < 8; p++)
            if (d >> p & 1)
                for (int i = p * 16 + 1; i < p * 16 + 16; i++) CRAM[i] = spr_bgpal[i];
        wx_dirty = 0;
    } else if (wx_dirty) {
        for (int i = 1; i < 128; i++)
            if ((i & 15) || !game_on || (i != 96 && i != 112)) CRAM[i] = wx_pal[i];   /* (96/112: HUD text) */
        wx_dirty = 0;
    }
    if (lamp_dirty) {
        for (int i = 0; i < 10; i++) CRAM[228 + i] = lamp_cols[i];
        lamp_dirty = 0;
    }
    if (glow_dirty) {
        for (int i = 0; i < 12; i++) CRAM[240 + i] = glow_cols[i];
        glow_dirty = 0;
    }
    if (glow_col != 0xFFFF) {
        CRAM[252] = glow_col;
        glow_col = 0xFFFF;
    }
}

static void present(int32_t camx, int32_t snes_y, uint16_t *frame_no)
{
    present2(camx, snes_y, frame_no, 0);
}

/* ------------------------------------------------------------ level loading */
static const pack_hdr_t *hdr;

#ifdef MUSDEBUG
/* stack high-water marks: fill the unused part of a stack, later count untouched words */
static void stack_fill(uint32_t lo)
{
    uint32_t sp;
    __asm__ volatile ("mov r15,%0" : "=r"(sp));
    for (volatile uint32_t *p = (volatile uint32_t *)UNCACHED(lo); (uint32_t)p < ((sp - 256) | 0x20000000); p++)
        *p = 0xA5A5A5A5;
}
static int stack_free(uint32_t lo)
{
    volatile uint32_t *p = (volatile uint32_t *)UNCACHED(lo);
    int n = 0;
    while (p[n] == 0xA5A5A5A5 && n < 1024) n++;
    return n * 4;
}
#endif

/* ask the slave for a song (0xFF = silence); wait until it switched (it no longer uses the old one) */
static uint32_t music_seq;
/* B + A cycles: 0 auto (full quality, half-rate voices if the level drops frames), 1 light, 2 off */
#ifdef NOMUSIC
static int music_mode = 2;
#else
static int music_mode = 0;
#endif
#define music_on (music_mode != 2)
static int music_msg;                     /* frames to show the music mode on the HUD */
static int level_seconds;
/* light: voices mixed at half rate (levels whose rendering needs both CPUs) */
static void music_play(int song, int light)
{
    if (!hdr->off_music) return;
    music_seq++;
    *UNCACHED(&music_req) = music_seq << 9 | (light ? 0x100 : 0) | (song & 0xFF);
    for (volatile int i = 0; i < 2000000 && *UNCACHED(&music_ack) != (music_seq & 0x7FFFFF); i++)
        ;
}

static int level_song(const level_t *lv)
{
    if (!hdr->off_music) return 0xFF;
    const mus_hdr_t *mh = PK(mus_hdr_t, hdr->off_music);
    return mh->level_song[lv->entrance & 0xFF];
}

static int menu_song(void)
{
#ifdef MENUSONG
    return MENUSONG;
#endif
    return hdr->off_music ? PK(mus_hdr_t, hdr->off_music)->menu_song : 0xFF;
}
static const level_t *cur_lv;
/* game mode: the music follows the game's own requests (CODE_B990CE: $0523 = song, $051D = 1;
   CODE_B990E7 stop: both 0) - bonus rooms, jingles, the map, cabins.  -2 = re-evaluate */
static int game_song = -2;
static int full_game;                    /* the complete game from power-on (pack has the boot snapshot) */
static int load_noplanes;                 /* (load_level: no Mega Drive planes) */
#ifdef PATHRING
volatile uint32_t path_ring[64][4] __attribute__((used)), path_ri __attribute__((used));
#define PATHLOG(c, v) do { volatile uint32_t *r_ = path_ring[path_ri++ & 63]; r_[0] = game_stats.frames; r_[1] = (c); r_[2] = (v); r_[3] = vcount; } while (0)
#else
#define PATHLOG(c, v) ((void)0)
#endif
static void music_follow(void)
{
    int want = 0xFF;
    if (music_on && (game_wram16(0x51D) & 0xFF)) want = game_wram16(0x523) & 0xFF;
    if (music_on && full_game) {
        /* the complete game: the music the game uploaded to its sound CPU last (the logos' fanfare
           and the intro / title theme are uploaded without $0523: it stays 0 = DK Island Swing);
           uploaded again: from the start */
        extern uint32_t game_apu_song, game_apu_song_n;
        static uint32_t seen_n;
        want = (int)game_apu_song;
        if (game_apu_song_n != seen_n) { seen_n = game_apu_song_n; game_song = -3; }
    }
    if (want == game_song) return;
    game_song = want;
    int s = 0xFF;
    if (want < 32 && hdr->off_music) s = PK(mus_hdr_t, hdr->off_music)->id_song[want];
    music_play(s, comp);
}

/* comp levels: store a 64-byte tile in the row-pair-interleaved layout */
static void comp_store(uint32_t slot, const uint32_t *t)
{
    for (int r = 0; r < 8; r++) {
        uint32_t *d = (uint32_t *)(TILE_B + slot * 16 + comp_roff(r));
        d[0] = t[r * 2]; d[1] = t[r * 2 + 1];
    }
}

static void bake_slot(const tileset_t *ts, uint32_t bk, uint32_t slot)
{
    uint32_t be = PK(uint32_t, ts->off_bake)[bk];
    uint32_t pidx = be >> 16, pal = ((be >> 8) & 0xFF) * 4, hf = be & 1;
    const uint8_t *src = PACK + ts->off_pool + pidx * 32;
    uint32_t tmp[16];
    uint8_t *dst = comp ? (uint8_t *)tmp : (uint8_t *)&tiles_ram[slot * 16];
    for (int r = 0; r < 8; r++) {
        for (int i = 0; i < 4; i++) {
            uint8_t b = src[r * 4 + i];
            uint8_t a = b >> 4, c = b & 15;
            uint8_t pa = a ? pal + a : 0, pc = c ? pal + c : 0;
            if (bake_remap) {
                pa = a ? bake_remap[pa] + bake_add : 0;
                pc = c ? bake_remap[pc] + bake_add : 0;
            }
            if (hf) { dst[r * 8 + 7 - i * 2] = pa; dst[r * 8 + 6 - i * 2] = pc; }
            else    { dst[r * 8 + i * 2] = pa; dst[r * 8 + i * 2 + 1] = pc; }
        }
    }
    uint8_t m = 0, f = 0;
    for (int r = 0; r < 8; r++) {
        const uint8_t *q = dst + r * 8;
        const uint32_t *w = (const uint32_t *)q;
        if (w[0] | w[1]) m |= 1 << r;
        if (q[0] && q[1] && q[2] && q[3] && q[4] && q[5] && q[6] && q[7]) f |= 1 << r;
    }
    rowmask[slot] = m;
    rowfull[slot] = f;
    if (comp) comp_store(slot, tmp);
}

static void set_masks(uint32_t slot, const uint32_t *tile)
{
    const uint8_t *dst = (const uint8_t *)tile;
    uint8_t m = 0, f = 0;
    for (int r = 0; r < 8; r++) {
        const uint8_t *q = dst + r * 8;
        int nz = 0;
        for (int i = 0; i < 8; i++) nz += q[i] != 0;
        if (nz) m |= 1 << r;
        if (nz == 8) f |= 1 << r;
    }
    rowmask[slot] = m;
    rowfull[slot] = f;
}

static uint32_t nslots;
static uint32_t slot_for(const tileset_t *ts, uint32_t bk)
{
#ifdef TCACHE
    (void)ts;
    return bk;                       /* tiles are baked on demand into the per-CPU caches */
#endif
    if (slotmap[bk] == 0xFFFF) {
        if (nslots >= MAX_SLOTS - 1) return 0;
        slotmap[bk] = nslots;
        bake_slot(ts, bk, nslots);
        nslots++;
    }
    return slotmap[bk];
}

static int cur_idx;

/* game logic: level-start snapshots (build/game/game_tab.c) */
static int game_on;
static uint32_t game_ticks;
/* frame skip (see the game loop): vcount at the last logic start, game frames run for the last
   picture, estimates (FRT ticks), on/off */
static uint32_t fs_vc;
static int32_t fs_l, fs_r, fs_done = 1, fs_rate;
#ifdef NO_FSKIP
static int fskip = 0;
#else
static int fskip = 1;
#endif
static int fg_on = 0;
#ifndef RIP_DEFAULT
#define RIP_DEFAULT 1
#endif
static int rip_on = RIP_DEFAULT;                    /* water levels: BG1 ripple (pause menu) */                     /* menu C: the foreground overlays (caves) - quality mode */
static uint16_t spr_ticks;
/* FRT ticks since (t0, vcount v0), also across several frames (the FRT wraps after ~1.4 frames) */
static uint32_t frt_since(uint16_t t0, uint32_t v0)
{
    uint32_t v, tv;
    uint16_t now;
    do { v = vcount; tv = t_vint; now = frt_read(); } while (v != vcount);
    if (v == v0) return (uint16_t)(now - t0);
    return (uint16_t)(tv - t0) + (v - v0 - 1) * FRAME_TICKS + (uint16_t)(now - tv);
}
#ifdef PERFREC
uint32_t frt_since_(uint16_t t0, uint32_t v0) { return frt_since(t0, v0); }
static inline uint16_t frt_read_(void) { return frt_read(); }
static inline uint32_t vcount_(void) { return vcount; }
#endif
#if defined(GAMEPROF) || defined(FEPHASE)
uint16_t ares_phase __attribute__((aligned(4)));
#endif
#ifdef PROFH
uint32_t prof_base, prof_other, prof_on;
uint16_t prof_hist[1024];
extern void prof_hint(void);
extern char gtext_start[];
static void prof_show(void)
{
    /* top 22 buckets as text, then stop */
    char ln[24];
    static const char hx[] = "0123456789ABCDEF";
    uint32_t total = prof_other;
    for (int i = 0; i < 1024; i++) total += prof_hist[i];
    REG8(SYS_INTMASK) = 0x08;
    COMM(C_CMD) = 0x8000 | 0x7FFF;        /* 68000: clear Genesis planes */
    while (COMM(C_CMD) & 0x8000)
        ;
    for (int i = 0; i < SCREEN_H; i++) sky_ram[i] = 0;
    static uint16_t h[1024];
    for (int i = 0; i < 1024; i++) h[i] = prof_hist[i];
    for (int page = 0; page < 4; page++) {
        uint16_t pick[22];
        for (int k = 0; k < 22; k++) {
            int best = 0;
            for (int i = 1; i < 1024; i++) if (h[i] > h[best]) best = i;
            pick[k] = best;
            h[best] = 0;
        }
        for (int b = 0; b < 2; b++) {
            for (int i = 0x100; i < 0x10000; i++) FB16[i] = 0x0101;
            write_line_table(0, 0);
            for (int k = 0; k < 22; k++) {
                int best = pick[k];
                uint32_t a = prof_base + best * 512;
                for (int j = 0; j < 8; j++) ln[j] = hx[(a >> (28 - 4 * j)) & 15];
                ln[8] = ' ';
                fmt_num(ln + 9, prof_hist[best] * 10000 / (total ? total : 1), 4);
                ln[13] = 0;
                text(8 + (k / 11) * 150, 8 + (k % 11) * 18, ln, 255);
            }
            ln[0] = 'O'; ln[1] = ' '; fmt_num(ln + 2, prof_other * 10000 / (total ? total : 1), 4); ln[6] = ' ';
            ln[7] = 'P'; ln[8] = '0' + page; ln[9] = 0;
            text(8, 210, ln, 255);
            wait_flip();
        }
        for (int f = 0; f < 240; f++) { volatile int d; for (d = 0; d < 60000; d++) ; }
    }
    for (;;) { CRAM[255] = 0xFFFF; CRAM[1] = 0x8000; CRAM[0] = 0; }
}
#endif
static uint32_t game_avg = 99;

/* ROM window 7 (cart 0x380000, SH-2 0x02380000) shows one 512 KB level bank (SSF2 mapper): the
   68000 writes the mapper register when asked (C_CMD 0xA000 | bank).  Both SH-2s drop their cached
   lines of the old bank (the slave on its next job: epoch). */
static uint32_t cur_bank = 7;
static void set_bank(uint32_t b)
{
    if (!b || b == cur_bank) return;
    {   /* F11 (MiSTer): never switch window 7 under a slave job that still reads the old bank
           (its last band / pipe frame); bounded (~2 frames) so a stuck slave cannot hang us */
        uint32_t v0 = vcount;
        while (*UNCACHED(&s_job_now) && vcount - v0 < 3)
            ;
    }
    COMM(C_CMD) = (uint16_t)(0xA000 | b);
    while (COMM(C_CMD) & 0x8000)
        ;
    cur_bank = b;
    CCR = 0x10;                            /* cache purge (CP) + back on */
    CCR = *UNCACHED(&comp) ? 0x19 : 0x11;
    UNCACHED(&shared_state)->epoch++;
}
/* all-worlds ROM: cart window 6 (SH-2 0x02300000) = the current world's bank (its snes_pg table +
   the world's SNES pages, mkgame.py world_split / tools/worldbanks.py).  Bank 6 (the default) =
   world bank 0.  Levels are entered only from their world's map, so the bank follows the map
   screen shown (map_setup) and the menu's level starts (game_level). */
static uint32_t cur_bank6 = 6;
static void set_bank6(uint32_t b)
{
    if (!b || b == cur_bank6) return;
    {   uint32_t v0 = vcount;
        while (*UNCACHED(&s_job_now) && vcount - v0 < 3)
            ;
    }
    COMM(C_CMD) = (uint16_t)(0xA100 | b);
    while (COMM(C_CMD) & 0x8000)
        ;
    cur_bank6 = b;
    CCR = 0x10;
    CCR = *UNCACHED(&comp) ? 0x19 : 0x11;
    UNCACHED(&shared_state)->epoch++;
}
static uint32_t level_bank6(int idx)
{
    for (uint32_t k = 0; k < hdr->nworld; k++) {
        const world_t *wd = PK(world_t, hdr->off_worlds) + k;
        for (uint32_t j = 0; j < wd->nlev; j++) if (wd->idx[j] == idx) return wd->pad[0];
    }
    return 0;
}
static void game_level(const level_t *lv)
{
    game_on = 0;
#ifdef NOGAME
    return;
#endif
    if (game_nscratch * 256 > (int)sizeof tiles_ram) return;     /* (build too small for the scratch pages) */
    for (int i = 0; i < 1; i++)
        if (lv->off_snap) {
            set_bank6(LV_BANK6(lv));
            set_bank(LV_BANK(lv));
            {   extern int game_keep_sram; game_keep_sram = 1; }
            game_init(PACK + lv->off_snap, lv->entrance);
            {   extern int game_keep_sram; game_keep_sram = 0; }
            {   /* (I2) the snapshot starts after the level's song upload: the song to follow is the
                   level's ($0523), not the last one uploaded before (the previous level's) */
                extern uint32_t game_apu_song, game_apu_song_n;
                game_apu_song = game_wram16(0x523) & 0xFF;
                game_apu_song_n++;
            }
            game_on = 1;
#ifdef STACKCHK
            {   /* paint the free RAM below the master stack: the game's C recursion depth */
                extern char _end[];
                uint32_t sp;
                __asm__ volatile ("mov r15,%0" : "=r"(sp));
                for (volatile uint32_t *p = (volatile uint32_t *)0x2603E810; (uint32_t)p < ((sp - 256) | 0x20000000); p++)
                    *p = 0xA5A5A5A5;
            }
#endif
            break;
        }
}

/* Genesis pad (S A C B R L D U) -> SNES $4218: A = Y (run / roll), B = B (jump), C = A, Start */
static uint32_t snes_pad(uint16_t pad)
{
    uint32_t v = 0;
    if (pad & PAD_UP) v |= SNES_UP;
    if (pad & PAD_DOWN) v |= SNES_DOWN;
    if (pad & PAD_LEFT) v |= SNES_LEFT;
    if (pad & PAD_RIGHT) v |= SNES_RIGHT;
    if (pad & PAD_A) v |= SNES_Y;
    if (pad & PAD_B) v |= SNES_B;
    if (pad & PAD_C) v |= SNES_A;
    if (pad & PAD_START) v |= SNES_START;
    return v;
}

/* (I17) the pad sample for the next logic frame; `left` = logic frames still to run for this picture,
   this one included (see pad_ring) */
static uint16_t pad_take(int left)
{
    uint32_t sr;
    __asm__ volatile ("stc sr,%0" : "=r"(sr));
    __asm__ volatile ("ldc %0,sr" :: "r"(sr | 0xF0) : "memory");
    pad_store();
    __asm__ volatile ("ldc %0,sr" :: "r"(sr) : "memory");
    if (vcount - pad_sv > 6) {                      /* (no numbered samples: the register as it is) */
        pad_tv = vcount;
        return PAD_NOW();
    }
    uint32_t n = pad_new, c = pad_cons;
    uint32_t avail = (n - c) & 0x3F;
    if (vcount - pad_tv > 8 || avail > 16) {        /* (no logic for a while: a load, a menu) */
        c = (n - 1) & 0x3F;
        avail = 1;
    }
    if (left < 1) left = 1;
    uint8_t prev = pad_ring[c & 15], add = 0;
    while (avail > (uint32_t)left) {                 /* behind: pass over the oldest, keep their presses */
        c = (c + 1) & 0x3F;
        uint8_t s = pad_ring[c & 15];
        add |= s & ~prev;
        prev = s;
        avail--;
    }
    if (avail) c = (c + 1) & 0x3F;                  /* (none new yet: the last one again) */
    pad_cons = c;
    pad_tv = vcount;
    uint16_t r = (uint16_t)(pad_ring[c & 15] | (add & (PAD_A | PAD_B | PAD_C | PAD_START)));
#ifdef PAD_OLD
    r = PAD_NOW();                        /* (test: the I16 behaviour - the register as it is) */
#endif
#ifdef PERFREC
    {   extern uint32_t perf_padb; static uint16_t pb; if ((r & PAD_B) && !(pb & PAD_B)) perf_padb++; pb = r; }
#endif
    return r;
}

static int game_reload;
static void fe_take(void);
static void load_level(int idx, shared_t *sh)
{
    fb_sync();                            /* (I17: an asynchronous swap first) */
    /* the 32X layer off while loading: the level palette goes to CRAM long before the canvas is
       redrawn (one frame of the old picture - a map screen - in the new colours, test 13).  (I13) The
       Genesis display first (68000 command 9D00, done at its next interrupt): the old level's planes
       showed alone until the 68000 got the new level's command (death, bonus rooms, level changes) */
    {
        uint32_t v0 = vcount;
        while ((COMM(C_CMD) & 0x8000) && vcount - v0 < 10)
            ;
        COMM(C_CMD) = 0x9D00;
        v0 = vcount;
        while ((COMM(C_CMD) & 0x8000) && vcount - v0 < 10)
            ;
    }
    REG16(VDP_DISPMODE) = 0;
    cur_idx = idx;
    {   /* (I21c) the Galleon boss: its distant island (OBJ tiles 140-14F, palette 6, priority 2) is drawn
           at a fixed 32 px right of the SNES picture, like its shadow band on the Genesis plane */
        extern int32_t spr_fixed_x;
        cf_view = (PK(level_t, hdr->off_levels) + idx)->entrance == 0x68;
        spr_fixed_x = cf_view ? 32 : 0x7FFF;
    }
    fe_take();                            /* (the front-end's map mirror lives in the slave's tile cache) */
    render_state_reset();                 /* (I1: the last screen's sprite state) */
    pv_avg[0] = pv_avg[1] = 0; pv_win = 0; pipe_off = 0;   /* (I7: 60 FPS mode measured again) */
    LDST(1);
    fs_r = 0;                             /* frame skip: new estimates */
    spr_rwbuf = sizeof map_mem >= 32768 ? (uint8_t *)map_mem + 16384 : 0;   /* CPU-written OBJ tiles (map font) while no level is shown (the lower half may hold the chunky tiles) */
    spr_rw_release();
    set_bank6(LV_BANK6(PK(level_t, hdr->off_levels) + idx));   /* (normally already: from its map) */
    LDST(2);
    set_bank(LV_BANK(PK(level_t, hdr->off_levels) + idx));
    if (!game_reload) {                   /* (the game already runs this level: no new start) */
        LDST(3);
        game_level(PK(level_t, hdr->off_levels) + idx);
    }
    cv_x0par = -1;
    dither_ripple = 0;
    fog3 = 0;
    const level_t *lv = PK(level_t, hdr->off_levels) + idx;
    int perf_fits = 0;
    if ((lv->flags & 4) && lv->off_fog) {
        /* the perf variant's fog tiles live in tiles_ram: in game builds (TCACHE) that is the small
           tile cache - Poison Pond (545 tiles) / Misty Mine (2 x 652) do not fit there and used to
           overwrite the variables behind it (game_on: a hang) -> read them from the pack (ROM) */
        const uint16_t *fb = PK(uint16_t, lv->off_fog);
        uint32_t ntl = fb[1] ? fb[0] * 2u : fb[0];
        perf_fits = ntl * 64 <= sizeof tiles_ram;
    }
    if ((lv->flags & 4) && comp_perf && lv->off_fog && lv->off_md2) {
        /* comp level, performance variant: BG1 + background on the Genesis planes (the 68000 streams
           BG1), the transparency layer here as a dithered canvas (mode 2) */
        cur_lv = lv;
        LDST(4);
        {   /* the previous level's effects off (the normal path below resets these too): left on, a
               lamp / rain / weather / light / millstone state drew garbage into the fog canvas */
            extern volatile int32_t spr_ms_on;
            sh->glow = 0; glow_t = -1; glow_peak = 0; lamp_scene = 0;
            sh->bm_lvl = sh->bm_on = sh->bm_game = 0;
            lamp_n = 0; lamp_img = 0;
            rain_n = 0; rain_cntA[0] = rain_cntA[1] = 0; sh->rcntB[0] = sh->rcntB[1] = 0;
            sh->job = 0;
            rain_state = 0; wx_k = 0; wx_t = 0; wx_on = 0; wx_dirty = 0; bg_mirror = 0; wx_first = 1;
            ws_on = 0; lt_on = 0;
            spr_ms_on = 0; ms_blk = 0; *UNCACHED(&ms_lvl) = 0;
        }
        music_play(0xFF, 0);
        LDST(5);
        COMM(C_CMD) = 0x8000 | 0x4000 | (game_on && !rip_on ? 0x2000 : 0) | idx;   /* (0x2000: no BG1 ripple, I14) */
        comp = comp_fog = comp_pal_dirty = 0;
        for (int b = 0; b < 2; b++) {     /* the fog bands leave rows / borders unpainted: no old picture there */
            for (int i = 256; i < 0x10000; i++) FB16[i] = 0;
            wait_flip();
        }
        REG16(VDP_DISPMODE) = 0;
        CCR = 0x10;
        CCR = 0x11;
        const uint16_t *fb = PK(uint16_t, lv->off_fog);
        uint32_t nt = fb[0], kind = fb[1];
        const uint16_t *fmap = fb + 2, *fpal = fb + 2 + 1024;
        const uint32_t *ft = (const uint32_t *)(fpal + 256);
        uint32_t ntl = kind ? nt * 2 : nt;                   /* kind 1: two baked dither phases */
        if (perf_fits)
            for (uint32_t i = 0; i < ntl * 16; i++) tiles_ram[i] = ft[i];
        *UNCACHED(&fog_tiles) = perf_fits ? tiles_ram : ft;
        fog3 = kind == 1;
        sh->fogn = nt;
        if (fog3) {
            mw_sway = (const uint8_t *)(ft + ntl * 16);
            mw_fresh = 1;
            mw_I = 0;
            cv_xbase = 0;
            for (int i = 0; i < 256; i++) comp_palbuf[i] = fpal[i];
        }
        LDST(6);
        for (int i = 0; i < 1024; i++) ovmap_ram[0][i] = fmap[i];
        pending_pal = 0;
        for (int i = 0; i < 256; i++) CRAM[i] = fpal[i];
        CRAM[253] = 0x83FF; CRAM[254] = 0x8000; CRAM[255] = 0xFFFF;
        if (game_on) spr_pal_dirty = 0xFF;            /* (I3: the game's sprite colours back at 128 +) */
        if (game_on) { CRAM[96] = 0x8000; CRAM[112] = 0xFFFF; }   /* (I11: the options menu's colours) */
#ifdef FOGDEBUG
        for (int i = 1; i < 253; i++) CRAM[i] = 0xFC1F;
#endif
        for (int i = 0; i < SCREEN_H; i++) sky_ram[i] = 0;
        ripple = !fog3 && (lv->flags & 2) != 0;
        if (game_on && !rip_on) ripple = 0;      /* (I14: the menu option, as for the other levels) */
        dither_ripple = 1;
        if (fog3) misty_palette();
        hint_mask = ripple ? 0x0C : 0x08;
        ripple_phase = 0;
        sft_show = 0;
        for (int i = 0; i < 64; i++)
            ripple_pat[i] = (i < 22) || (i >= 33 && i < 43);
        sh->grid = PK(uint16_t, lv->off_grid);
        sh->gw = lv->gw;
        sh->gh = lv->gh;
        sh->ox0 = sh->oy0 = 0;
        sh->nov = 0;
        sh->ovcap = sh->glow = 0;
        sh->cv = fog3 ? 3 : 2;
        CV3_ND[0] = CV3_ND[1] = 0;        /* (I9: no sprite rectangles to restore yet) */
        cvb[0].valid = cvb[1].valid = 0;
        cvb[0].hud = cvb[1].hud = (trect_t){ 0, 0, 0, 0 };   /* (the last level's HUD text rectangles) */
        spr_clear_marks(0); spr_clear_marks(1);            /* (and its sprites' restore marks) */
        sh->cvrows[0] = sh->cvrows[1] = 0;
        sh->split = SCREEN_H / 2;
        sh->split_row = SPLIT_ROW;
        sh->epoch++;
        sh->menu = 0;
        anim_frame = 0;
        LDST(7);
        while (COMM(C_CMD) & 0x8000)
            ;
        LDST(8);
        if (game_on) disp_pending = 2;    /* (I13: with the Genesis planes, as below) */
        else REG16(VDP_DISPMODE) = 0x0001;
        *UNCACHED(&music_light) = music_mode == 1;
        game_song = -2;                       /* (the game's song: music_follow) */
        level_seconds = 0;
        return;
    }
    const tileset_t *ts = PK(tileset_t, lv->off_ts);
    const uint16_t *grid = PK(uint16_t, lv->off_grid);
    const uint16_t *vch = PK(uint16_t, ts->off_vch);
    cur_lv = lv;
    {   /* (G1) BG1 tile priorities for OBJ priority 2 (sprites.c tile8_behind) */
        extern const uint16_t *volatile spr_prio_grid, *volatile spr_prio_mask;
        extern volatile int32_t spr_prio_gw, spr_prio_gh;
        *UNCACHED(&spr_prio_grid) = grid;
        *UNCACHED(&spr_prio_gw) = lv->gw;
        *UNCACHED(&spr_prio_gh) = lv->gh;
        *UNCACHED(&spr_prio_mask) = ts->off_vprio ? PK(uint16_t, ts->off_vprio) : 0;
    }
    music_play(0xFF, 0);                     /* silence: frees the echo buffer (shared with slotmap) */

    COMM(C_CMD) = 0x8000 | (load_noplanes ? 0x7FFF : idx);   /* 68000 loads the Genesis planes meanwhile
                                                             (complete game's power-on: none - H2) */
    comp = (lv->flags & 4) != 0;
    comp_fog = (lv->flags & 8) != 0;
    comp_pal_dirty = 0;
    if (comp) {                           /* comp levels leave the side borders untouched: clear */
        for (int b = 0; b < 2; b++) {
            for (int i = 256; i < 0x10000; i++) FB16[i] = 0;
            wait_flip();
        }
    }
    REG16(VDP_DISPMODE) = 0;              /* blank the 32X while baking */

    bake_remap = 0;
    if (comp) {
        /* base layer record carries the CGRAM -> quantized colour table after its map */
        const uint16_t *bm = PK(uint16_t, lv->ov[0].off_map);
        bake_remap = (const uint8_t *)(bm + (uint32_t)lv->ov[0].w * lv->ov[0].h);
    }
    CCR = 0x10;                           /* purge; comp levels use two-way cache + 2KB on-chip RAM */
    CCR = comp ? 0x19 : 0x11;
    if (comp) {
        static const uint32_t z[16];
        comp_store(ZERO_SLOT, z);
        rowmask[ZERO_SLOT] = 0;
    }
#ifndef TCACHE
    for (uint32_t i = 0; i < ts->nbake && i < MAX_BAKE; i++) slotmap[i] = 0xFFFF;
#endif
    comp_U = (lv->flags >> 8) & 0xFF;
    bake_add = 0;                                         /* (old BG1-overdraw perf mode retired) */
    for (uint32_t i = 0; i < sizeof(vused); i++) vused[i] = 0;
    nslots = 0;
    uint32_t n = (uint32_t)lv->gw * lv->gh;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = grid[i];
        vused[v >> 3] |= 1 << (v & 7);
    }
#ifdef TCACHE
    sh->vch = vch;
    sh->ts = ts;
    sh->tcd = tiles_ram; sh->tct = tc_mtag; sh->tcm = TC_MMASK; sh->tcn = TC_MN;
    { volatile uint16_t *tg = tc_mtag; for (int i = 0; i < TC_MN; i++) tg[i] = 0xFFFF; }
    for (int i = 0; i < TC_SN; i++) tc_stag[i] = 0xFFFF;
    for (uint32_t bk = 0; bk < ts->nbake && bk < MAX_BAKE; bk++) {
        /* row masks by bake index (the opaque pixels do not depend on the palette or the flip) */
        const uint8_t *src = PACK + ts->off_pool + (PK(uint32_t, ts->off_bake)[bk] >> 16) * 32;
        uint8_t m = 0, f = 0;
        if (PK(uint32_t, ts->off_bake)[bk] & 2) { rowmask[bk] = 0xFF; rowfull[bk] = 0; continue; }   /* (composite) */
        for (int r = 0; r < 8; r++) {
            uint32_t w = (uint32_t)src[r * 4] << 24 | src[r * 4 + 1] << 16 | src[r * 4 + 2] << 8 | src[r * 4 + 3];
            uint32_t nz = (w | w >> 1 | w >> 2 | w >> 3) & 0x11111111u;
            if (nz) m |= 1 << r;
            if (nz == 0x11111111u) f |= 1 << r;
        }
        rowmask[bk] = m; rowfull[bk] = f;
    }
#else
    sh->vch = vch_ram;
    for (uint32_t v = 0; v < ts->nvch && v < MAX_VCH; v++) {
        if (!(vused[v >> 3] & (1 << (v & 7)))) continue;
        for (int k = 0; k < 16; k++) {
            uint16_t e = vch[v * 16 + k];
            vch_ram[v * 16 + k] = slot_for(ts, e & 0x7FFF) | (e & 0x8000);
        }
    }
#endif
    bake_add = 0;
    for (uint32_t k = 0; k < lv->nov && k < 2; k++) {
        ovinfo[k] = lv->ov[k];
        const uint16_t *m = PK(uint16_t, lv->ov[k].off_map);
        uint32_t nmap = (uint32_t)lv->ov[k].w * lv->ov[k].h;
        if (comp && k == 1) {
            /* light layer: pre-baked add-value tiles (k * S per pixel) copied into slots */
            const uint16_t *ex = m + nmap;
            uint32_t nlt = ex[0];
            const uint32_t *lt = (const uint32_t *)(ex + 2);
            uint32_t s0 = nslots;
            for (uint32_t i = 0; i < nlt && nslots < MAX_SLOTS - 1; i++, nslots++) {
                set_masks(nslots, lt + i * 16);
                comp_store(nslots, lt + i * 16);
            }
            for (uint32_t i = 0; i < nmap; i++)
                ovmap_ram[k][i] = (s0 + (m[i] & 0x7FFF)) | (m[i] & 0x8000);
            if (ex[1]) {
                /* fog wave data: sway[64], u16 S, u16 K, u16 qcol[S], u8 vrep2[K] */
                const uint8_t *w = (const uint8_t *)(lt + nlt * 16);
                mw_sway = w;
                mw_S = ((const uint16_t *)(w + 64))[0];
                mw_K = ((const uint16_t *)(w + 64))[1];
                mw_qcol = (const uint16_t *)(w + 68);
                mw_vrep2 = w + 68 + 2 * mw_S;
            }
        } else if (lv->ov[k].xm == 4) {
            /* heavy snow: flake lists live in the pack (read through the cache from ROM) */
            const uint8_t *tab = (const uint8_t *)(m + nmap);
            for (int i = 0; i < 64; i++) snow_sway[i] = tab[i];
            for (int i = 0; i < 32; i++) snow_wob[i] = (int8_t)tab[64 + i];
            const uint32_t *fo = (const uint32_t *)(tab + 96);
            for (int f = 0; f < 8; f++) flake_frame[f] = (const uint8_t *)m + fo[f];
            anim_n[k] = 0;
        } else {
            for (uint32_t i = 0; i < nmap; i++)
                ovmap_ram[k][i] = m[i] == 0x7FFF ? 0x7FFF : (slot_for(ts, m[i] & 0x7FFF) | (m[i] & 0x8000));
        }
    }
    if (comp) {
        /* empty tiles -> the zero slot, so the line compositor's row setup needs no mask lookups */
#ifndef TCACHE
        for (uint32_t v = 0; v < ts->nvch && v < MAX_VCH; v++) {
            if (!(vused[v >> 3] & (1 << (v & 7)))) continue;
            for (int k = 0; k < 16; k++) {
                uint16_t e = vch_ram[v * 16 + k];
                if (!rowmask[e & 0x7FFF]) vch_ram[v * 16 + k] = ZERO_SLOT;
                else if (rowfull[e & 0x7FFF] == 0xFF) vch_ram[v * 16 + k] = e | 0x4000;
            }
        }
#endif
        /* in place: layer 1's u32 map lies past both u16 maps; layer 0 converts backwards */
        for (int k = 1; k >= 0; k--) {
            uint32_t nmap = (uint32_t)lv->ov[k].w * lv->ov[k].h;
            for (int32_t i = nmap - 1; i >= 0; i--) {
                uint16_t e = ovmap_ram[k][i];
                if (e == 0x7FFF || !rowmask[e & 0x7FFF]) e = ZERO_SLOT;
                comp_map[k][i] = TILE_B + (e & 0x7FFF) * 16 + ((e & 0x8000) ? COMP_FLIPC + 1 : 0);
            }
        }
    }
    anim_frame = 0;
#ifdef PROF
    prof_nslots = nslots;
#endif
    const uint16_t *pal = PK(uint16_t, lv->off_pal);
    pending_pal = 0;                      /* never let a queued menu palette overwrite the level's */
    for (int i = 0; i < 256; i++) CRAM[i] = pal[i];
    if (game_on) spr_pal_dirty = 0xFF;    /* the game's sprite palettes (a reload keeps spr_pal) */
#ifdef PALDBG
    for (int i = 1; i < 253; i++) if (i & 15) CRAM[i] = 0x801F;   /* (test: the 32X layer in red) */
#endif
    CRAM[253] = 0x83FF; CRAM[254] = 0x8000; CRAM[255] = 0xFFFF;
    if (game_on) { CRAM[96] = 0x8000; CRAM[112] = 0xFFFF; hud_on_c = 0x7070; hud_off_c = 0x6060; }
    else { hud_on_c = 0xFFFF; hud_off_c = 0xFEFE; }
    const uint16_t *sky = PK(uint16_t, lv->off_sky);
    for (int i = 0; i < SCREEN_H; i++) sky_ram[i] = sky[i];

    ripple = (lv->flags & 2) != 0;
    if (game_on && !rip_on) ripple = 0;   /* (menu option; the sprites are drawn against the
                                             ripple, sprites.c tile8r) */
    {
        int sc = 1;
        for (int i = 1; i < SCREEN_H; i++) if (sky[i] != sky[0]) sc = 0;
        hint_mask = (sc && (comp || !ripple)) ? 0x08 : 0x0C;   /* no per-line work: V-int only */
    }
    if (comp_fog) {
        mw_I = 0;
        mw_fresh = 1;
        misty_palette();
    }
    ripple_phase = 0;
    sft_show = 0;
    for (int i = 0; i < 64; i++)
        ripple_pat[i] = (i < 22) || (i >= 33 && i < 43);
    sh->grid = grid;
    sh->gw = lv->gw;
    sh->gh = lv->gh;
    sh->ox0 = lv->cx0 * 32;
    sh->oy0 = lv->cy0 * 32;
    sh->nov = lv->nov > 2 ? 2 : lv->nov;
    if (game_on && !fg_on) sh->nov = 0;   /* performance mode: no foreground overlay (caves) */
#ifdef NOCANVAS
    sh->cv = 0;
#else
    sh->cv = !comp;
    for (int k = 0; k < sh->nov; k++)
        if (ovinfo[k].xm == 4) sh->cv = 0;           /* heavy snow: flakes everywhere, full redraws */
#ifdef NOCANVASOV
    if (sh->nov) sh->cv = 0;
#endif
#endif
    sh->ovcap = sh->nov;
    sh->ov_rows = 8;
    sh->glow = 0;
    glow_t = -1;
    glow_peak = 0;
    lamp_scene = 0;
    sh->bm_lvl = sh->bm_on = sh->bm_game = 0;
    lamp_n = 0;
    lamp_img = 0;
    rain_n = 0;
    rain_cntA[0] = rain_cntA[1] = 0;
    sh->rcntB[0] = sh->rcntB[1] = 0;
    sh->job = 0;
    rain_state = 0; wx_k = 0; wx_t = 0; wx_on = 0; wx_dirty = 0; bg_mirror = 0; wx_first = 1;
    ws_on = 0;
    lt_on = 0;
    {   /* temple levels: the millstone picture (block kind 7, sprites.c spr_ms_draw) */
        extern volatile int32_t spr_ms_on;
        extern const uint32_t *spr_ms_tiles;
        extern int32_t spr_ms_c0, spr_ms_r0, spr_ms_c1, spr_ms_r1;
        extern void spr_ms_palette(uint32_t pal);
        spr_ms_on = 0;
        ms_blk = 0;
        {   extern const uint32_t *spr_ms_opq; extern volatile int32_t spr_ms_qpos[2][3], spr_ms_cq[4];
            spr_ms_opq = 0;
            for (int i = 0; i < 2; i++) *UNCACHED(&spr_ms_qpos[i][2]) = 0;
            *UNCACHED(&spr_ms_cq[2]) = 0;
            *UNCACHED(&ms_lvl) = 0; }
        if ((lv->flags & 0x10) && lv->off_fog && PK(uint16_t, lv->off_fog)[1] == 7) {
            ms_blk = PK(uint16_t, lv->off_fog);
            spr_ms_palette(ms_blk[3]);
            spr_ms_c0 = ms_blk[4]; spr_ms_r0 = ms_blk[5]; spr_ms_c1 = ms_blk[6]; spr_ms_r1 = ms_blk[7];
            {   /* cells, opaque mask and tiles copied to SDRAM (map_mem + 17920: temple levels have no
                   rain / overlays) - cartridge ROM reads made the picture cost ~20% of a frame */
                extern const uint32_t *spr_ms_opq;
                extern volatile uint32_t spr_ms_gen;
                extern const uint16_t *volatile spr_ms_cells;
                uint32_t nb = 4 * 448 * 2 + 4 * 14 * 4 + 4 * 28 + (uint32_t)ms_blk[2] * 32;
                const uint16_t *src = ms_blk + 8;
                if (17920 + nb <= sizeof map_mem) {
                    uint32_t *d = (uint32_t *)((uint8_t *)map_mem + 17920);
                    for (uint32_t i = 0; i < nb / 4; i++) d[i] = ((const uint32_t *)src)[i];
                    src = (const uint16_t *)d;
                }
                ms_cells0 = src;
                spr_ms_cells = src;
                extern const uint8_t *spr_ms_span;
                spr_ms_opq = (const uint32_t *)(src + 4 * 448);
                spr_ms_span = (const uint8_t *)(spr_ms_opq + 4 * 14);
                spr_ms_tiles = (const uint32_t *)(spr_ms_span + 4 * 28);
                *UNCACHED(&spr_ms_gen) = *UNCACHED(&spr_ms_gen) + 1; }
            for (int i = 0; i < 2; i++)
                for (int r = 0; r < 32; r++) { uint32_t *d = UNCACHED(&ms_defer[i][r][0]); d[0] = d[1] = 0; }
            *UNCACHED(&ms_lvl) = 1;
        }
    }
    if ((lv->flags & 0x10) && lv->off_fog && PK(uint16_t, lv->off_fog)[1] == 6) {
        const uint16_t *wb = PK(uint16_t, lv->off_fog);
        lt_n = wb[2];
        lt_flicker = wb[3];
        lt_bar = wb + 8;
        lt_state = lt_timer = lt_flick = 0;
        lt_prevc = (int32_t)0x80000000;            /* first frame: no crossing test */
        ws_start = ws_cur = ws_from = ws_to = 0;
        ws_ntrig = 0;
        ws_shift = wb[5];
        ws_pkeys = lt_bar + 2 * lt_n + wb[4] * 64;
        ws_skeys = ws_pkeys + wb[4] * 128;
        ws_k = ws_t = 0;
        ws_on = lt_on = 1;
        lt_game = !wb[6];
        COMM(10) = wb[6] ? 0x40FF : 0x4000;         /* (beam levels: apex message, 0xFF = off) */
        if (wb[6]) {                                /* Torchlight: Squawks' beam, lit palette at 128 */
            const uint16_t *lit = ws_skeys + wb[4];
            sh->bm_lvl = 1;
            bm_ax0 = wb[6];
            bm_ay0 = wb[7];
            sh->bm_game = game_on; sh->bm_kind = 2;
            if (game_on) bm_game_setup(lit, ws_pkeys);
            else
            for (int i = 1; i < 126; i++) CRAM[128 + i] = (i & 15) ? lit[i] : 0;   /* (253: HUD yellow) */
#ifdef BEAMDBG
            for (int i = 1; i < 126; i++) {                                     /* (test: lit = white, */
                CRAM[128 + i] = 0xFFFF;                                         /*  dark = red) */
                if (i & 15) CRAM[i] = 0x801F;
            }
#endif
        }
    }
    if ((lv->flags & 0x10) && lv->off_fog && PK(uint16_t, lv->off_fog)[1] == 5) {
        const uint16_t *wb = PK(uint16_t, lv->off_fog);
        ws_start = ws_cur = ws_from = ws_to = wb[2];
        ws_ntrig = wb[3];
        ws_shift = wb[5];
        ws_trig = wb + 8;
        ws_pkeys = ws_trig + 2 * ws_ntrig + wb[4] * 64;
        ws_skeys = ws_pkeys + wb[4] * 128;
        ws_k = ws_t = 0;
        ws_on = 1;
        ws_heavy(sh, ws_start == 5);
        sh->ovcap = 0;
        COMM(10) = 0x4000 | ws_start << 11 | ws_start << 8;
    }
    if ((lv->flags & 0x10) && lv->off_fog && sh->cv && PK(uint16_t, lv->off_fog)[1] == 4) {
        const uint16_t *rb = PK(uint16_t, lv->off_fog);
        rain_n = rb[0];
        wx_rain_x = rb[2];
        wx_dark_x = rb[3];
        for (int i = 0; i < 3; i++) {
            rain_ci[i] = (uint8_t)(game_on ? 16 + 16 * i : 245 + i);
            CRAM[rain_ci[i]] = rb[4 + i];
        }
        wx_palN = PK(uint16_t, lv->off_pal);
        wx_skyN = PK(uint16_t, lv->off_sky);
        wx_palD = rb + 136;
        wx_skyD = rb + 264;
        rain_pix = (const uint8_t *)(rb + 712);       /* after the BG3 haze target (224) */
        wx_on = 1;
        /* game mode: mirror the game's colours (they start as the level-start snapshot's = the pack's) */
        bg_mirror = game_on && rb[7] == 0;
        { extern uint32_t mrd16(uint32_t); bg_flash_col = mrd16(0xB9946A); }
        bg_sky_t = 0;
#ifdef NORAIN
        rain_state = 2;
#endif
    } else if ((lv->flags & 0x10) && lv->off_fog && sh->cv && PK(uint16_t, lv->off_fog)[1] == 3) {
        /* hanging lamps: shade + light cone image, baked into the canvas by lamp_row */
        const uint16_t *lb = PK(uint16_t, lv->off_fog);
        lamp_n = lb[2];
        *UNCACHED(&lamp_noshade) = game_on;     /* (G2: the game draws the swinging lamp itself) */
        lamp_h = lb[3];
        lamp_pos = lb + 4;
        const uint16_t *lpal = lamp_pos + 2 * lamp_n;
        lamp_w = lpal + 22;                         /* (shade colours, light levels) */
        lamp_img = (const uint8_t *)(lpal + 25);
        for (int i = 0; i < 8; i++) CRAM[128 + 16 * i] = lamp_pal[i] = lpal[i];   /* (sprite colour 0 slots) */
        lamp_scene = lpal + 12;                     /* cone light levels: colours set by lamp_frame */
        lamp_d = 99;
        sh->nov = 0;
    } else if ((lv->flags & 0x10) && lv->off_fog && sh->cv && PK(uint16_t, lv->off_fog)[1] == 2) {
        /* Slipslide glimmer: the BG3 map as overlays 0/1 (one per framebuffer), drawn as a 50% checkerboard
           only while a glimmer is on */
        const uint16_t *gb = PK(uint16_t, lv->off_fog);
        uint32_t n = gb[0], base = nslots;
        const uint32_t *gt = (const uint32_t *)(gb + 2 + 1024);
        glow_mean = gb[2 + 1024 + n * 32];               /* after the n tiles of 64 bytes */
        glow_peak = gb + 2 + 1024 + n * 32 + 1;
        for (uint32_t i = 0; i < n && nslots < MAX_SLOTS - 1; i++, nslots++) {
            for (int k = 0; k < 16; k++) tiles_ram[nslots * 16 + k] = gt[i * 16 + k];
            set_masks(nslots, gt + i * 16);
        }
        for (int i = 0; i < 1024; i++) {
            ovmap_ram[0][i] = base + gb[2 + i];
            ovmap_ram[1][i] = base + gb[2 + i];         /* (the checker phase is applied as drawn) */
        }
        for (int k = 0; k < 2; k++) {
            ovinfo[k].w = ovinfo[k].h = 32;
            ovinfo[k].xm = ovinfo[k].ym = 3;
            ovinfo[k].xs = ovinfo[k].ys = 0;
            ovinfo[k].ysub = 0; ovinfo[k].yclamp = 0xFFFF;
        }
        sh->ovcap = 1;
        sh->glow = 1;
        sh->nov = 0;
        glow_t = 0x60;
    }
    cvb[0].valid = cvb[1].valid = 0;
        cvb[0].hud = cvb[1].hud = (trect_t){ 0, 0, 0, 0 };   /* (the last level's HUD text rectangles) */
    /* game sprites: the chunky tile copy in map_mem's lower half, if this level leaves it free */
    spr_ck_attach((game_on && sizeof map_mem >= 32768 && !sh->nov && !sh->ovcap && !rain_n && !lamp_n && !comp && !(sh->bm_lvl && !sh->bm_game))
                  ? (uint32_t *)map_mem : 0);
    sh->cvrows[0] = sh->cvrows[1] = 0;
    sh->split = SCREEN_H / 2;
    sh->split_row = SPLIT_ROW;
    for (int i = 0; i < SCREEN_H / 4; i++) comp_ltP[0][i] = comp_ltP[1][i] = 0xFF;
    ring_x0 = ring_y0 = -0x10000;
    sh->epoch++;
    sh->menu = 0;

    while (COMM(C_CMD) & 0x8000)          /* wait for the 68000 */
        ;
    if (game_on) disp_pending = 2;          /* (on at the second flip of the new level: with the
                                               Genesis planes, md/main.c disp_hold) */
    else REG16(VDP_DISPMODE) = 0x0001;
    *UNCACHED(&music_light) = music_mode == 1;
    if (game_on) game_song = -2;            /* (the game's song: music_follow) */
    else music_play(music_on ? level_song(lv) : 0xFF, comp);
    level_seconds = 0;
}

/* ------------------------------------------------------------ menu */
static void draw_menu(int w, int sel)
{
    const world_t *wd = PK(world_t, hdr->off_worlds) + w;
    set_bank(wd->bank);
    const uint8_t *img = PACK + wd->off_img;
    write_line_table(0, 0);
    /* borders + map */
    for (int y = 0; y < SCREEN_H; y++) {
        volatile uint32_t *d = FB32 + (FB_BASE + y * STRIDE) / 4;
        const uint32_t *s = (const uint32_t *)(img + y * 256);
        for (int i = 0; i < 8; i++) d[i] = 0;
        for (int i = 0; i < 64; i++) d[8 + i] = s[i];
        for (int i = 72; i < 84; i++) d[i] = 0;
    }
    /* world name */
    int tw = text_width(wd->name);
    box(32, 0, 256, 16, 254);
    text(160 - tw / 2, 3, wd->name, 253);
    /* level banner */
    const level_t *lv = PK(level_t, hdr->off_levels) + wd->idx[sel];
    box(32, 186, 256, 34, 254);
    tw = text_width(lv->name);
    text(160 - tw / 2, 190, lv->name, 255);
    text(40, 190, "<", 253);
    text(274, 190, ">", 253);
    char buf[12] = "LEVEL x / y";
    buf[6] = '1' + sel;
    buf[10] = '0' + wd->nlev;
    text(160 - text_width(buf) / 2, 205, buf, 253);
    text(40, 205, fskip ? "SKIP ON" : "SKIP OFF", 253);     /* B: frame skip under overload */
    text(222, 205, fg_on ? "FG ON" : "FG OFF", 253);          /* C: cave foregrounds (quality) */
    text(4, 92, "^", 253);
    text(4, 104, "v", 253);
}

/* ---- the game's world map screen (WAI 80E90E: after a death, between levels): our world map image
   (the menu's) in canvas layout - line y at FB_BASE + y * CV_P + 64, SNES x at screen x + 32 - and
   the game's sprites over it (restored under their marks each frame) */
static int map_mode;
static int full_game;                    /* the complete game from power-on (pack has the boot snapshot) */
#ifndef MAP_FAST
#define MAP_FAST 3                       /* game frames per picture while the Kongs walk on a map */
#endif
static const uint8_t *map_img;
static int map_black;                     /* a screen without a picture here (Cranky's cabin...): sprites on black */
static void map_rows(int32_t y0, int32_t y1, int32_t c0, int32_t c1)
{
    /* canvas tile columns c0..c1-1 (screen x = 8c - 32 .. ), lines y0..y1-1 */
    for (int32_t y = y0; y < y1; y++) {
        volatile uint32_t *d = FB32 + (FB_BASE + y * CV_P + CV_XOFF) / 4;
        const uint32_t *s = (const uint32_t *)(map_img + y * 256);
        for (int32_t c = c0; c < c1; c++) {
            int32_t ix = c * 8 - 32;                 /* image x of this column (screen x 32 = image 0) */
            if (map_black || ix < 0 || ix >= 256 || y >= SCREEN_H) { d[c * 2] = 0; d[c * 2 + 1] = 0; }
            else { d[c * 2] = s[ix / 4]; d[c * 2 + 1] = s[ix / 4 + 1]; }
        }
    }
}
static int map_mi = -1;                  /* the map screen shown (-1: a world's picture) */
static int cab_txt = -1;                 /* a cabin's dialogue is drawn: its h offset (else -1) */
static int map_cab = -1;
#ifdef TCACHE
/* the dialogue drawn in each frame buffer: in the slave's tile cache memory, lent while no level is
   shown (fescr.c fe_lend: cabins are entered from a map); the dialogue itself = the front-end VRAM
   mirror (sprites.c fe_vm, words 7600-77FF: BG3 map $7400 rows 16-31, low byte = ASCII) */
#define cab_drawn ((uint8_t (*)[16][32])((uint8_t *)tc_sdata + 8320))
#define CAB_CH(r, c) (fe_vm && (FE_TRK->vmask >> (6 + ((r) >> 3)) & 1) && fe_vm[0x600 + (r) * 32 + (c)] != 0xFFFF ? fe_vm[0x600 + (r) * 32 + (c)] & 0xFF : 0x20)
#else
static uint8_t cab_drawn[2][16][32];
#define CAB_CH(r, c) 0x20
#endif
static void cab_colours(const uint16_t *pal);
static void map_setup(int world)
{
    const world_t *wd = PK(world_t, hdr->off_worlds) + (world >= 0 ? world : 0);
    uint32_t off_img = wd->off_img, off_pal = wd->off_pal, bank = wd->bank;
    map_mi = -1;
    cab_txt = -1;
    map_cab = -1;
    render_state_reset();                 /* (H11 / I4: no level's priority mask / ripple bands) */
    if (world <= -100) {                  /* -100 - k: cabin k (Cranky, Funky, Candy) */
        const cabin_t *cb = PK(cabin_t, hdr->off_screens) + (-100 - world);
        off_img = cb->off_img; off_pal = cb->off_pal; bank = cb->bank;
        cab_txt = cb->txh;
        map_cab = -100 - world;
        for (int b = 0; b < 2; b++) for (int r = 0; r < 16; r++) for (int c = 0; c < 32; c++) cab_drawn[b][r][c] = ' ';
    } else if (world < 0) {               /* -1 - mi: the game's map screen mi, when in this build */
        const mapscr_t *ms = PK(mapscr_t, hdr->off_maps) + (-1 - world);
        off_img = ms->off_img; off_pal = ms->off_pal; bank = ms->bank;
        map_mi = -1 - world;
        set_bank6(ms->pad[0]);            /* (all-worlds ROM: that map's world bank) */
    } else
        set_bank6(wd->pad[0]);
    set_bank(bank);
    if (map_cab >= 0) cab_colours(PK(uint16_t, off_pal));   /* (I14: after the bank switch) */
    UNCACHED(&shared_state)->epoch++;      /* (F11: the slave drops its cache lines even without a bank change) */
    map_img = PACK + off_img;
    COMM(C_CMD) = 0x8000 | 0x7FFF;           /* 68000: clear the Genesis planes */
    while (COMM(C_CMD) & 0x8000)
        ;
    /* (I13) black (opaque: CRAM 0x8000, the sky lines too) until the new picture is on screen: the
       sky colour of a map is transparent (the Genesis backdrop), so setting it first showed the last
       screen's picture as backdrop-coloured silhouettes while this one was drawn (a few frames on
       the MiSTer: the white / brown flashes entering the island map and Kongo Jungle's map) */
    for (int i = 0; i < SCREEN_H; i++) *UNCACHED(&sky_ram[i]) = 0x8000;
    for (int i = 0; i < 256; i++) CRAM[i] = 0x8000;
    const uint16_t *pal = PK(uint16_t, off_pal);
    pending_pal = 0;
    spr_pal_dirty = 0xFF;
    cv_x0par = -1;
    ripple = 0;
    sft_show = 0;
    for (int b = 0; b < 2; b++) {
        for (int y = 0; y < 256; y++) FB16[y] = (uint16_t)((FB_BASE + y * CV_P + CV_XOFF) >> 1);
        map_rows(0, SCREEN_H, 0, CV_P / 8);
        spr_clear_marks(cur_fb ^ 1);
        wait_flip();
    }
    /* (right after the flip: in the vblank) both buffers hold the new picture now */
    upload_palette(pal);
    menu_sky(pal);
    cvb[0].valid = cvb[1].valid = 0;
        cvb[0].hud = cvb[1].hud = (trect_t){ 0, 0, 0, 0 };   /* (the last level's HUD text rectangles) */
}
/* a cabin's dialogue (the game's BG3 map rows 16-31, ASCII from $20; font colour 1 = white letters,
   2 = their black edge; the SNES adds the layer onto BG1 with HDMA'd colours: white here).  Cells whose
   letter changed, or that the sprite restore redrew, are drawn again. */
/* a cabin's dialogue (the game's BG3 map rows 16-31, ASCII from $20; font colour 1 = white letters,
   2 = their black edge; the SNES adds the layer onto BG1 and OBJ with HDMA'd colours: white here).
   Whole text rows: a row with a changed letter gets its picture band back first; a row the sprite
   restore (old marks) or the sprites (new marks, pass 2: text in front) touched is drawn again.
   (Cranky's layer is 4 px off the 8 px canvas columns: per-cell redraws cut the neighbours.) */
static int cab_y0(int r) { return 16 + r >= 23 ? 176 + (16 + r - 23) * 12 : (16 + r) * 8 - 8; }
static int cab_row_marked(int r, const uint32_t (*mk)[2])
{
    int32_t y0 = cab_y0(r), a = y0 < 0 ? 0 : y0 >> 3, b = (y0 + 7) >> 3;
    for (int32_t rr = a; rr <= b && rr < 28; rr++) if (mk[rr][0] | mk[rr][1]) return 1;
    return 0;
}
/* (I14) the dialogue's white / black: the cabin picture's own brightest / darkest colours (CRAM 1-127).
   CRAM 254 / 255 are sprite palette 7's colours 14 / 15 (Funky's board was black there) */
static uint8_t cab_white = 255, cab_black = 254;
static void cab_colours(const uint16_t *pal)
{
    int bw = -1, bb = 1000;
    cab_white = 255; cab_black = 254;
    for (int i = 1; i < 128; i++) {
        if (!(i & 15)) continue;
        uint32_t c = pal[i];
        int l = (int)(c & 31) * 2 + (int)((c >> 5) & 31) * 4 + (int)((c >> 10) & 31);
        if (l > bw) { bw = l; cab_white = (uint8_t)i; }
        if (l < bb) { bb = l; cab_black = (uint8_t)i; }
    }
}
static void cab_glyphs(int r)
{
    const uint8_t *font = PK(uint8_t, hdr->off_tfont);
    int32_t y0 = cab_y0(r);
    for (int c = 0; c < 32; c++) {
        uint32_t ch = CAB_CH(r, c);
        if (ch < 0x21 || ch >= 0x80) continue;
        int32_t x0 = c * 8 - cab_txt + 32;        /* canvas x (picture x 0 = 32) */
        const uint8_t *g = font + (ch - 0x20) * 16;
        for (int y = 0; y < 8; y++) {
            int32_t sy = y0 + y;
            if (sy < 0 || sy >= SCREEN_H) continue;
            uint32_t p0 = g[y * 2], p1 = g[y * 2 + 1];
            volatile uint8_t *d = FB8 + FB_BASE + sy * CV_P + CV_XOFF + x0;
            for (int x = 0; x < 8; x++) {
                uint32_t v = (p0 >> (7 - x) & 1) | (p1 >> (7 - x) & 1) << 1;
                if (v == 1) d[x] = cab_white; else if (v) d[x] = cab_black;
            }
        }
    }
}
/* pass 1 (before the sprites, after the restore under the old marks) */
static void cab_text1(int bi, const uint32_t (*old)[2])
{
    for (int r = 0; r < 16; r++) {
        int32_t y0 = cab_y0(r);
        if (y0 + 8 <= 0 || y0 >= SCREEN_H) continue;
        int changed = 0;
        for (int c = 0; c < 32; c++) {
            uint32_t ch = CAB_CH(r, c);
            if (ch != cab_drawn[bi][r][c]) { changed = 1; cab_drawn[bi][r][c] = (uint8_t)ch; }
        }
        if (changed) {
            int32_t ya = y0 < 0 ? 0 : y0, yb = y0 + 8 > SCREEN_H ? SCREEN_H : y0 + 8;
            map_rows(ya, yb, 0, 44);
        }
        if (changed || cab_row_marked(r, old)) cab_glyphs(r);
    }
}
/* pass 2 (after the sprites): rows under sprites again, in front of them */
static void cab_text2(const uint32_t (*mk)[2])
{
    for (int r = 0; r < 16; r++) {
        int32_t y0 = cab_y0(r);
        if (y0 + 8 <= 0 || y0 >= SCREEN_H) continue;
        if (cab_row_marked(r, mk)) cab_glyphs(r);
    }
}
static void map_frame(void)
{
    int bi = cur_fb ^ 1;
    const uint32_t (*mk)[2] = (const uint32_t (*)[2])((uint32_t)&spr_marks[bi][0][0] | 0x20000000);
    for (int r = 0; r < 28; r++) {
        uint32_t m0 = mk[r][0], m1 = mk[r][1];
        for (int c = 0; c < 44; c++)
            if (((c & 32) ? m1 : m0) & bit32[c & 31]) map_rows(r * 8, r * 8 + 8, c, c + 1);
    }
    if (cab_txt != -1) cab_text1(bi, mk);   /* (before the marks are cleared: it reads them) */
    spr_clear_marks(bi);
    spr_draw_band(0, SCREEN_H, bi, 32, 0);
    if (cab_txt != -1) cab_text2(mk);       /* (the marks of the sprites just drawn) */
}

/* ---- battery save: the game's 2 KB SRAM <-> the cartridge's (md/main.c sram_xfer: a word at a time
   through COMM0 / COMM2 / COMM4).  Layout (bytes): "DK32", checksum (16-bit sum of the data), its
   complement, the 2048 data bytes.  Only while no level is drawn (MiSTer maps SRAM over the cart's
   upper half meanwhile: nothing may read the ROM there - the slave is idle, this code and its data are
   below 2 MB / in SDRAM). */
#define SRAM_WORDS 1028
static int sram_ok;                       /* the cartridge's save was loaded at power-on */
/* (I18b) hash of the save data the cartridge holds (after the load / the last save): the game writes
   its SRAM during play too ($6057 = $61 then $60 again, e.g. when a bonus room is entered) - the
   content does not change, so no save is made (each one stopped the 68000 for a moment and, on
   MiSTer, a save at a bonus room's entry was followed by a hang) */
static uint32_t sram_hash_saved = 0xFFFFFFFFu;
static uint32_t sram_hash(const uint8_t *d)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < 2048; i++) h = (h ^ d[i]) * 16777619u;
    return h;
}
static int sram_wait(volatile uint16_t *r, uint16_t v)
{
    uint32_t v0 = vcount;
    while (*r != v)
        if (vcount - v0 > 120) return 0;  /* (no answer: an old 68000 program?) */
    return 1;
}
static void sram_save(void)
{
    const uint8_t *d = game_sram();
    uint32_t h = sram_hash(d);
    if (h == sram_hash_saved) { game_sram_dirty = 0; return; }   /* (I18b: nothing new) */
    {   /* (I18b) only with the 68000 idle: a command still running would overwrite 0x9F01;
           (I18c) and the slave idle - on MiSTer the cartridge SRAM covers the whole upper cart
           (the level / front-end banks) while it is on: a slave job reading them then drew
           garbage tiles (that stayed in the tile caches) */
        uint32_t v0 = vcount;
        while ((COMM(C_CMD) & 0x8000) || *UNCACHED(&s_job_now) || COMM(C_DONE) != COMM(C_TOKEN))
            if (vcount - v0 > 120) return;
    }
    uint32_t sum = 0;
    for (int i = 0; i < 2048; i++) sum += d[i];
    COMM(2) = 0; COMM(4) = 0;
    COMM(C_CMD) = 0x9F01;
    if (!sram_wait(&COMM(C_CMD), 0x9FF0)) return;
    for (int i = 0; i < SRAM_WORDS; i++) {
        uint32_t w;
        switch (i) {
        case 0: w = 'D' << 8 | 'K'; break;
        case 1: w = '3' << 8 | '2'; break;
        case 2: w = sum & 0xFFFF; break;
        case 3: w = ~sum & 0xFFFF; break;
        default: w = d[(i - 4) * 2] << 8 | d[(i - 4) * 2 + 1]; break;
        }
        COMM(0) = (uint16_t)w;
        COMM(2) = (uint16_t)(i + 1);
        if (!sram_wait(&COMM(4), (uint16_t)(i + 1))) return;
    }
    COMM(2) = 0xFFFF;
    sram_wait(&COMM(C_CMD), 0);
    game_sram_dirty = 0;
    sram_hash_saved = h;
    /* (I18c) whatever was read from the upper cart meanwhile is not kept: both CPUs drop their caches
       (the slave on its next job: epoch) */
    CCR = 0x10;
    CCR = *UNCACHED(&comp) ? 0x19 : 0x11;
    UNCACHED(&shared_state)->epoch++;
}
/* -> 1 if the cartridge held a valid save (copied into the game's SRAM) */
static int sram_load(void)
{
    static uint8_t buf[8];
    uint8_t *d = game_sram();
    COMM(2) = 0; COMM(4) = 0;
    COMM(C_CMD) = 0x9F02;
    if (!sram_wait(&COMM(C_CMD), 0x9FF0)) return 0;
    /* (straight into a scratch copy: the game's SRAM only if the checksum holds) */
    uint8_t *tmp = (uint8_t *)map_mem;     /* (no level yet / none shown) */
    for (int i = 0; i < SRAM_WORDS; i++) {
        if (!sram_wait(&COMM(2), (uint16_t)(i + 1))) return 0;
        uint32_t w = COMM(0);
        if (i < 4) { buf[i * 2] = (uint8_t)(w >> 8); buf[i * 2 + 1] = (uint8_t)w; }
        else { tmp[(i - 4) * 2] = (uint8_t)(w >> 8); tmp[(i - 4) * 2 + 1] = (uint8_t)w; }
        COMM(4) = (uint16_t)(i + 1);
    }
    sram_wait(&COMM(C_CMD), 0);
    uint32_t sum = 0;
    for (int i = 0; i < 2048; i++) sum += tmp[i];
    if (buf[0] != 'D' || buf[1] != 'K' || buf[2] != '3' || buf[3] != '2') return 0;
    if ((buf[4] << 8 | buf[5]) != (sum & 0xFFFF) || (buf[6] << 8 | buf[7]) != (~sum & 0xFFFF)) return 0;
    for (int i = 0; i < 2048; i++) d[i] = tmp[i];
    return 1;
}

/* (I13) the slave job token: m_main's jobs and fescr.c's sprite job count up the same number.  (fescr.c
   used COMM(C_TOKEN) ^ 0x8000: its job left COMM(C_DONE) at a value m_main's own token never has, so
   the next wait of m_main for an idle slave without a job of its own in between - the first game frame
   after the world map, "give" wait - never ended: STUCK page, M STAGE 3, entering Jungle Hijinx from
   the map in PicoDrive, I12) */
static uint16_t job_token;

#ifdef TCACHE
#include "fescr.c"
#else
static void fe_lend(void) {}
static void fe_take(void) {}
static int fe_find(void) { return -1; }
static int fe_idx = -1;
static uint32_t fe_palsum;
static void fe_setup(int i) { (void)i; }
static void fe_frame(uint16_t *f) { (void)f; }
static uint32_t fe_ent_bank6(uint32_t e) { (void)e; return 0; }
static int fe_same(void) { return 0; }
static void fe_lend_early(void) {}
#endif

/* ------------------------------------------------------------ master */
volatile uint32_t sdgame_fixes;          /* (I13) words of the SDRAM game code that needed a second copy */
void m_main(void)
{
    {   /* (I12) the recompiled game code that runs from SDRAM (mars.ld .sdgame): its image follows the .data
           image in the first 256 KB of the cartridge; the BIOS copies only .data, so this copies the rest
           before anything runs it.  Uncached writes, then the cache purged. */
        extern uint32_t sdgame_start[], sdgame_end[];   /* (mars.ld: _sdgame_*) */
        extern char sdgame_load[];
        const uint32_t nw = (uint32_t)(sdgame_end - sdgame_start);
        if ((uint32_t)sdgame_load >= 0x300000) {
            /* (I17) the image is in world bank 0 (cart window 6, bank 6 at power-on): its header
               (finalize.py) must be there.  After a reset in another world the 68000's md_setup
               switches window 6 back to bank 6 - wait for that (uncached reads) */
            const volatile uint32_t *h = (const volatile uint32_t *)(0x22000000 + (uint32_t)sdgame_load - 16);
            for (uint32_t t = 0; ; t++) {
                if (h[0] == 0x5344474D && h[1] == nw * 4 && (h[2] ^ h[3]) == 0xFFFFFFFF) break;
                if (t > 20000000) for (;;) CRAM[0] = 0x801F;     /* (red screen: no image) */
            }
        }
        /* (I13) copied, then read back and compared (a few ms once): a word that did not arrive is
           copied again (up to 4 rounds) instead of running wrong code */
        for (int round = 0; round < 4; round++) {
            const volatile uint32_t *src = (const volatile uint32_t *)(0x22000000 + (uint32_t)sdgame_load);
            volatile uint32_t *dst = (volatile uint32_t *)((uint32_t)sdgame_start | 0x20000000);
            uint32_t bad = 0;
            for (uint32_t n = nw; n; n--, src++, dst++) {
                uint32_t v = *src;
                if (round == 0 || *dst != v) { *dst = v; bad += round != 0; }
            }
            if (round) sdgame_fixes += bad;
            if (round && !bad) break;
        }
        if ((uint32_t)sdgame_load >= 0x300000) {   /* (I17) the copy against the header's sum */
            const volatile uint32_t *h = (const volatile uint32_t *)(0x22000000 + (uint32_t)sdgame_load - 16);
            const volatile uint32_t *dst = (const volatile uint32_t *)((uint32_t)sdgame_start | 0x20000000);
            uint32_t sm = 0;
            for (uint32_t n = 0; n < nw; n++) sm += dst[n];
            if (sm != h[2]) for (;;) CRAM[0] = 0x801F;
        }
        CCR = 0x10;
        CCR = 0x11;
    }
#ifdef TCACHE
    {   /* the game's load-only WRAM pages alias the master's tile cache data (mkgame.py) */
        extern const int game_nscratch;
        if (game_nscratch * 256 > TC_MN * 64) for (;;) CRAM[0] = 0x801F;     /* (red screen: build error) */
        extern const int game_nring;
        if (game_nring * 256 > (int)sizeof ring_ent) for (;;) CRAM[0] = 0x801F;
    }
#endif
    {   /* the recompiled game code addresses WRAM 0-1FFF through GBR (r65mem.h) */
        extern uint8_t wram_lo[];
        __builtin_set_thread_pointer(wram_lo);
    }
    shared_t *sh = UNCACHED(&shared_state);
    hdr = PK(pack_hdr_t, 0);
    font = PACK + hdr->off_font;

    FRT_TCR = 0;
    canary_set();
#ifdef MUSDEBUG
    stack_fill(0x0603E810);
#endif
    {
        uint16_t a = frt_read();
        for (volatile int i = 0; i < 200; i++)
            ;
        frt_ok = frt_read() != a;
    }
    while (!(REG16(SYS_ADAPTER) & 0x8000))
        ;
    pal = !(REG16(VDP_DISPMODE) & 0x8000);     /* 32X VDP: bit 15 = 1 NTSC, 0 PAL */
#ifdef FORCEPAL
    pal = 1;
#endif
    if (pal) frame_ticks = 57348;
    REG16(VDP_DISPMODE) = 0;
    for (int i = 0; i < SCREEN_H; i++) sky_ram[i] = 0;
    CRAM[0] = 0;
    REG16(VDP_DISPMODE) = 0x0001;
    for (int b = 0; b < 2; b++) {
        for (int i = 0; i < 0x10000; i++) FB16[i] = 0;
        write_line_table(0, 0);
        wait_flip();
    }
#ifdef DEBUGSTAGES
#define STAGE(c) do { for (int i_ = 0; i_ < SCREEN_H; i_++) sky_ram[i_] = (c); } while (0)
#else
#define STAGE(c) do { } while (0)
#endif
#ifdef BENCH
    bench();
#endif
    STAGE(0x001F);
    REG8(SYS_HCOUNT) = 0;
    REG8(SYS_INTMASK) = 0x0C;
    STAGE(0x03E0);

    COMM(C_CMD) = 0x8000 | 0x7FFF;        /* 68000: clear Genesis planes */
    while (COMM(C_CMD) & 0x8000)
        ;
    STAGE(0x7C00);

    int world = 0, sel = 0, dirty = 2, in_level = 0;
    int32_t camx = 0, camy = 1;
    uint16_t frame_no = 0, prev_pad = 0;
#define token job_token                  /* (I13: one counter with fescr.c's sprite job) */
#ifdef NOHUD
    int autoscroll = 0, show_hud = 0;
#else
    int autoscroll = 0, show_hud = 1;
#endif
    int early = 0;
    int spr_defer = 0;                    /* (I17) 60 FPS mode: spr_prepare runs after the slave's kick */
#ifndef PIPE_SPRDEFER
#define PIPE_SPRDEFER 1
#endif
    int32_t j3_logic = -1;                /* (I17) this frame's logic time, for the early job's checks */
    int32_t pcamx = 0, pcamy = 1;          /* last frame's camera (strip prediction) */
    trect_t pwin = { 0, 0, 0, 0 };
    uint32_t fps = 0, swaps = 0, fps_mark = vcount;
    int m_pct = 0, s_pct = 0;
    uint16_t hud_ticks = 0;
    char line[24];
    int go_menu = 0, go_dirty = 0;        /* GAME OVER: its screen, then back to the menu */
    int blanked = 0;                      /* the game's screen is off: our palette is all black */
    const world_t *worlds = PK(world_t, hdr->off_worlds);
    sh->menu = 1;
    while (world + 1 < (int)hdr->nworld && !worlds[world].nlev) world++;   /* (the first world in this build) */
    set_bank(worlds[world].bank);
    pending_pal = PK(uint16_t, worlds[world].off_pal);
    pending_sky = 1;
#ifndef AUTOLEVEL
    music_play(music_on ? menu_song() : 0xFF, 0);
#endif

#ifndef AUTOLEVEL
    /* the complete game: power-on snapshot (logos, title, file select, maps, levels, ending) - the
       default.  Hold START while switching on for the level viewer menu (-DFULLGAME: never the menu,
       -DVIEWERMENU: always) */
#if defined(FULLGAME)
    if (hdr->off_boot) {
#elif defined(VIEWERMENU)
    if (0) {
#else
    if (hdr->off_boot && !(COMM(C_PAD) & PAD_START)) {
#endif
        full_game = 1;
        show_hud = 0;                      /* (the game: no frame-rate line) */
        load_noplanes = 1;
        load_level(0, sh);                 /* (a renderer state; the game shows its title screens first) */
        load_noplanes = 0;
        COMM(C_CMD) = 0x8000 | 0x7FFF;     /* (H2: and not its Mega Drive planes - the jungle at night
                                              flashed at power-on while the 32X layer was off) */
        while (COMM(C_CMD) & 0x8000)
            ;
        disp_pending = 0;                  /* (32X layer off until the first front-end picture: fescr.c) */
        REG16(VDP_DISPMODE) = 0;
        for (int i = 0; i < 256; i++) CRAM[i] = 0x8000;
        fe_lend();                         /* (before the game sets up its first screen) */
        fe_lend_hook = fe_lend_early;      /* (and mid-frame when a dark screen writes its maps) */
        game_init(PACK + hdr->off_boot, 0x100);
#ifdef STACKCHK
        {   /* (test) paint the free RAM below the master stack (read back from SDRAM dumps) */
            extern char _end[];
            uint32_t sp;
            __asm__ volatile ("mov r15,%0" : "=r"(sp));
            for (volatile uint32_t *p = (volatile uint32_t *)0x2603E810; (uint32_t)p < ((sp - 256) | 0x20000000); p++)
                *p = 0xA5A5A5A5;
        }
#endif
        sram_ok = sram_load();             /* (the battery save, if the cartridge has one) */
        sram_hash_saved = sram_hash(game_sram());   /* (I18b: what the cartridge holds now - a blank
                                              cartridge is saved once the game writes real data) */
        game_sram_dirty = 0;
#ifdef SRAMTEST
        game_sram_dirty = 1;               /* (test: write it back at once) */
#endif
        /* the snapshot is taken after the boot code set up the logos (VRAM, PPU / HDMA registers the
           snapshot does not hold): run that setup (CODE_80BA2C, entered from reset) again instead of
           the first NMI - the logos start over from their first frame */
        game_debug_jump(0x0080BA2C);
        game_on = 1;
        in_level = 1;
        game_fblank = 1;
#ifdef ENDTEST
        {   /* (test: the ending - Gang-Plank Galleon from its snapshot, then at game frame 30 the
               "K. Rool beaten" code CODE_B884D7: Cranky's speech, the hoard, the credits) */
            int gi = 0;
            for (uint32_t k = 0; k < hdr->nlev; k++) if (PK(level_t, hdr->off_levels)[k].entrance == 0x68) gi = (int)k;
            game_debug_jump(0);
            load_level(gi, sh);
            game_fblank = 0;
        }
#endif
        camx = cur_lv->startx - 64;
        camy = cur_lv->starty + 1;
        autoscroll = 0;
    }
#endif
#ifdef AUTOLEVEL
#ifdef HANGANY
    hang_vbl = 0; hang_armed = 1;            /* (test: the watchdog also over the load) */
#endif
#ifdef PERFREC
    pipe_mode = (int)perf_cfg[2];
    fskip = !(perf_cfg[3] & 1);
    if (perf_cfg[3] & 8) flip_async = 0;
#endif
    load_level(AUTOLEVEL, sh);              /* test builds: straight into a level, auto-scrolling */
    in_level = 1;
    camx = cur_lv->startx - 64;
    camy = cur_lv->starty + 1;
    autoscroll = 1;
#ifdef CAMX
    camx = CAMX; camy = CAMY; autoscroll = 0;
#endif
#ifdef STARTX
    camx = STARTX;
#endif
#ifdef NOSCROLL
    autoscroll = 0;
#endif
#endif
    prev_pad = PAD_NOW();                 /* (I12: the Start held while switching on is not a press - it picked
                                             the menu's first level at once) */
    for (;;) {
        hang_vbl = 0;
#ifdef HANGANY
        hang_armed = in_level;
#else
#ifdef HANGBOOT
        hang_armed = in_level && game_on;
#else
        hang_armed = in_level && game_on && !(full_game && game_stats.frames < 2);   /* (the boot setup frame is long) */
#endif
#endif
        uint16_t pad = PAD_NOW();
        uint16_t press = pad & ~prev_pad;
        prev_pad = pad;
        if (spr_defer) { spr_defer = 0; spr_prepare(); }   /* (I17: a path that skipped the picture) */
        PERF_MARK(11);
        uint16_t t0 = frt_read();
        uint32_t vc_t0 = vcount;
        uint32_t fb_wait_ticks = 0;       /* (I17) this iteration's wait for an async swap (not render work) */
        pipe_async = 0;
        PT(0);
        { static uint16_t lc; LDST(0x100 | (++lc & 0xFF)); }
        if (canary_bad()) hang_show();    /* (I1: a stack ran over: stop here, visibly) */
#ifdef NO32XDISP
        if (in_level) REG16(VDP_DISPMODE) = 0;   /* (test: Genesis layers only) */
#endif

        if (!in_level) {
            fb_sync();
            const world_t *wd = &worlds[world];
            if (press & PAD_LEFT)  { sel = sel ? sel - 1 : wd->nlev - 1; dirty = 2; }
            if (press & PAD_RIGHT) { sel = (sel + 1) % wd->nlev; dirty = 2; }
            if (press & (PAD_UP | PAD_DOWN)) {
                for (uint32_t k = 0; k < hdr->nworld; k++) {     /* (worlds without levels in this build: skipped) */
                    world = (press & PAD_DOWN) ? (world + 1) % hdr->nworld : (world ? world - 1 : hdr->nworld - 1);
                    if (worlds[world].nlev) break;
                }
                sel = 0;
                dirty = 2;
                set_bank(worlds[world].bank); pending_pal = PK(uint16_t, worlds[world].off_pal);
                pending_sky = 1;
            }
            if (press & PAD_B) { fskip ^= 1; dirty = 2; }
            if (press & PAD_C) { fg_on ^= 1; dirty = 2; }
            if (press & (PAD_A | PAD_START)) {
                start_block = (press & PAD_START) != 0;
                load_level(wd->idx[sel], sh);
                in_level = 1;
                camx = cur_lv->startx - 64;
                camy = cur_lv->starty + 1;
                autoscroll = 0;
                continue;
            }
            if (dirty) {
                draw_menu(world, sel);
                dirty--;
            }
#ifdef MUSDEBUG
            {
                static uint32_t last_n, mark;
                static char b[24] = "-";
                if (vcount - mark >= (pal ? 50u : 60u)) {
                    mark = vcount;
                    uint32_t n = *UNCACHED(&music_refills);
                    fmt_num(b, (int)(n - last_n), 4); b[4] = ' ';
                    extern volatile uint32_t mp_eng_ticks, dsp_prof[4];
                    static uint32_t lp0, lp1;
                    uint32_t p0 = UNCACHED(&dsp_prof[0])[0], p1 = UNCACHED(&dsp_prof[0])[1];
                    uint32_t nn = n - last_n ? n - last_n : 1;
                    fmt_num(b + 5, (int)*UNCACHED(&music_ticks), 5); b[10] = ' ';
                    fmt_num(b + 11, (int)((p0 - lp0) / nn), 5); b[16] = ' ';
                    fmt_num(b + 17, (int)((p1 - lp1) / nn), 5); b[22] = 0;
                    lp0 = p0; lp1 = p1;
                    static uint32_t lt;
                    uint32_t tt = *UNCACHED(&music_ticks);
                    fmt_num(b + 5, (int)((tt - lt) / nn), 5);
                    lt = tt;
                    last_n = n;
                }
                box(32, 170, 200, 14, 254);
                text(36, 172, b, 255);
                {
                    static char c[20];
                    fmt_num(c, stack_free(0x0603E810), 4); c[4] = ' ';
                    fmt_num(c + 5, stack_free(0x0603F800), 4); c[9] = ' ';
                    extern volatile uint32_t music_bench;
                    fmt_num(c + 10, (int)*UNCACHED(&music_bench), 5); c[15] = 0;
                    box(32, 154, 120, 14, 254);
                    text(36, 156, c, 255);
                }
            }
#endif
            present(0, 0, &frame_no);
            continue;
        }

        /* ---- in level */
#ifdef AUTOCYCLE
        {   /* test builds: every level in turn */
            static uint32_t fr;
            if (++fr == AUTOCYCLE) {
                fr = 0;
#ifdef AUTOCYCLE_SAME
                load_level(cur_idx, sh);
#else
                load_level((cur_idx + 1) % hdr->nlev, sh);
#endif
                camx = cur_lv->startx - 64;
                camy = cur_lv->starty + 1;
                continue;
            }
        }
#endif
        if (game_on) {
            /* the game: one frame of SNES logic with this pad; its camera drives the renderer */
            PERF_MARK(7);
            uint16_t g0 = frt_read();
            uint32_t gv0 = vcount, gt0 = gtick;
            uint32_t sp = snes_pad(pad);
            /* frame skip: under a sustained overload (one logic frame + the render no longer fit in a
               frame) run two (three) logic frames per picture, so the game keeps its speed at 30 (20)
               pictures per second instead of slowing down; single late frames are not caught up (the
               catch-up frame would be late too).  Every logic frame still runs: same physics. */
            int fsteps = 1;
            {
                int32_t owed = (int32_t)(gt0 - fs_vc);     /* 60 Hz ticks since the last logic start */
                fs_vc = gt0;
                if (fskip && frt_ok && owed >= 2 && owed <= 3 && fs_r) {
                    /* (MiSTer jungle: L 73 %, rest 46 %: 2 L + R = 192 % fits in 2 frames - a 94 %
                       margin picked 3 steps = 20 pictures/s) */
                    if (fs_l + fs_r > FRAME_TICKS * 97 / 100) {
                        fsteps = 2;
                        if (owed >= 3 && 2 * fs_l + fs_r > 2 * FRAME_TICKS) fsteps = 3;
                    }
                }
                {   /* (perf) pictures that keep taking 2+ vblanks although the estimates say they fit
                       (underwater levels ran at exactly half speed until the first death): after 3 such
                       pictures in a row, catch up the vblanks that passed */
                    static int late;
                    late = owed >= 2 && owed <= 3 ? late + 1 : 0;
                    if (fskip && late >= 3 && fsteps < owed && !game_fblank && game_in_main_loop()) fsteps = owed;
                }
                if (fskip && af_last && owed >= 2 && !game_fblank && game_in_main_loop()) {
                    /* (I17) gameplay pictures with FLIP_ASYNC: most swaps go on during the next logic
                       (no vblank wait per picture), so catching up every vblank that passed does not
                       snowball - the game keeps exactly 60 logic frames a second */
                    fsteps = owed > 3 ? 3 : owed;
                }
                if (fskip && pipe_pend && owed >= 2) {
                    /* 60 FPS mode: the picture is shown a frame later and its timing jitters with the
                       logic - catch up every vblank that passed, so the game always runs 60 logic
                       frames a second (the estimates above let it drift into slow motion) */
                    fsteps = owed > 3 ? 3 : owed;
                }
                if (pal && owed > fsteps && owed <= 6)   /* (more: a load / pause, not lag) */
                    fsteps = owed > 3 ? 3 : owed;   /* PAL: keep 60 logic frames a second */
                if (map_mode == 1 && (game_wram16(0xE89) | game_wram16(0xEF1)))
                    fsteps = MAP_FAST;            /* (G12) the map Kong walking (MapDK X/Y speed): faster */
                {   /* front-end screens (fescr.c): keep the game's pace (light logic; a picture under big
                       sprites can take 4-6 frames).  (I21) What one picture cannot catch up (12 logic frames)
                       is carried to the next pictures: a screen's set-up and first picture (the Nintendo
                       logo after the Rare logo: ~22 vblanks) put the game - and the next screen - behind
                       the music, which runs in real time */
                    static int32_t fe_debt;
                    if (map_mode == 4 && owed >= 1 && owed <= 64) {
                        int32_t want = owed + fe_debt;
                        if (want > 64) want = 64;
                        fsteps = want > 12 ? 12 : want;
                        fe_debt = want - fsteps;
                    } else fe_debt = 0;
                }
#ifdef FS_FORCE
                fsteps = FS_FORCE;                /* (test: always skip) */
#endif
                PATHLOG(8, (uint32_t)owed | (uint32_t)fsteps << 8 | (uint32_t)map_mode << 16);
#ifdef PERFREC
                {   extern uint32_t perf_lost[4];        /* (vblanks without a logic frame: after an async / a sync swap) */
                    if (owed > fsteps && owed <= 8) perf_lost[af_last ? 0 : 1] += (uint32_t)(owed - fsteps);
                    if (owed > 8) perf_lost[3]++; }
#endif
            }
            dbg_stage = 1;
            early = 0;
            /* (I12) paused with the menu drawn in both buffers: the picture does not change - no drawing
               (each picture redrew the menu box: 4-6 vblanks in Torchlight, the game's unpause wait of
               16 frames took seconds) */
            if (!pm_open) pm_shown = 0;
            pm_frz = pm_open && pm_shown >= 2 && pm_key == PM_KEY();
            if (sh->cv == 1 && !pm_frz && !ripple && !dither_ripple && cvb[cur_fb ^ 1].valid && !pipe_pend && !game_fblank && !fe_vm) {
                /* (not while no level is shown: the slave's tile cache memory is lent to fescr.c then) */
                /* the slave restores the canvas under this buffer's old sprites meanwhile */
                int bi = cur_fb ^ 1;
                LDST2(1); while (COMM(C_DONE) != token)
                    ;
                sh->pw[0] = cvb[bi].w.x0; sh->pw[1] = cvb[bi].w.x1; sh->pw[2] = cvb[bi].w.y0; sh->pw[3] = cvb[bi].w.y1;
                sh->pbi = bi;
                /* ... and under its last HUD text (cv_band then has none to restore) */
                if (hud_early) {
                    sh->ph[0] = cvb[bi].hud.x0; sh->ph[1] = cvb[bi].hud.x1; sh->ph[2] = cvb[bi].hud.y0; sh->ph[3] = cvb[bi].hud.y1;
                    cvb[bi].hud = (trect_t){ 0, 0, 0, 0 };
                } else {
                    sh->ph[0] = sh->ph[1] = sh->ph[2] = sh->ph[3] = 0;
                }
                sh->pdraw = 0;
#ifdef NO_EARLY_OV
                if (!sh->ovcap && !sh->nov && !sh->bm_lvl && !rain_n && !lamp_n) {
#else
                if (!sh->bm_lvl && !rain_n && !lamp_n && !sh->glow) {
#endif
                    /* ... and draws the tiles entering the window at the predicted camera (last move
                       repeated); kept only within 1 tile of the old window and of the real one, so the
                       drawn rows / columns stay inside the canvas (32 x 44 tiles for a 30 x 42 window) */
                    int32_t px = 2 * camx - pcamx, py = 2 * camy - pcamy;
                    int32_t gx = CV_GX(sh, px), gy = CV_GY(sh, py);
                    trect_t P = { gx >> 3, (gx >> 3) + CV_TW, gy >> 3, (gy >> 3) + CV_TH };
                    trect_t W = cvb[bi].w;
                    if (P.x0 - W.x0 <= 1 && W.x0 - P.x0 <= 1 && P.y0 - W.y0 <= 1 && W.y0 - P.y0 <= 1) {
                        sh->cvN[0] = P.x0; sh->cvN[1] = P.x1; sh->cvN[2] = P.y0; sh->cvN[3] = P.y1;
                        sh->cvI[0] = P.x0 > W.x0 ? P.x0 : W.x0; sh->cvI[1] = P.x1 < W.x1 ? P.x1 : W.x1;
                        sh->cvI[2] = P.y0 > W.y0 ? P.y0 : W.y0; sh->cvI[3] = P.y1 < W.y1 ? P.y1 : W.y1;
                        sh->cvH[0] = sh->cvH[1] = sh->cvH[2] = sh->cvH[3] = 0;
                        sh->cvfull = 0;
                        sh->cvbi = bi;
                        sh->pdraw = 1;
                        pwin = P;
                    }
                }
                sh->job = 3;
                token++;
                COMM(C_TOKEN) = token;
                early = 1;
            }
#ifdef PROFH
            prof_on = 1;
#endif
#if defined(GAMEPROF) && !defined(PHASE_SPR) && !defined(PHASE_BG)
            *(volatile uint16_t *)&ares_phase = 1;      /* (ares profiler: game logic phase) */
#endif
            PT(1);
            {   /* the options menu while the game is paused (Start); the game sees only Start / Select */
                int paused = (game_wram16(0x579) & 0x40) && game_in_main_loop() && !game_fblank;
                /* (I14) the Start that paused: with frame skip the game can get it in a picture's
                   second logic frame (the freshest pad) - one picture before `press` sees the edge.
                   That edge then queued a resume: the menu closed by itself ~30 frames later
                   (Torchlight, any level running 2 logic frames per picture).  Ignored until released. */
                static int pm_hold;
                if (paused && !pm_open) { pm_open = 1; pm_sel = 0; pm_hold = ((pad | press) & PAD_START) != 0; }
                if (!paused) { pm_open = 0; pm_req = 0; pm_hold = 0; }
                if (pm_hold && !(pad & PAD_START)) pm_hold = 0;
                if (pm_open) {
                    uint32_t g = 0;
                    if (press & PAD_UP) pm_sel = (pm_sel + PM_N - 1) % PM_N;
                    if (press & PAD_DOWN) pm_sel = (pm_sel + 1) % PM_N;
                    if (press & (PAD_A | PAD_B | PAD_C)) {
                        switch (pm_sel) {
                        case PM_RESUME: g = SNES_START; break;
                        case PM_FSKIP: fskip ^= 1; break;
                        case PM_PIPE: pipe_mode ^= 1; break;
                        case PM_FG: fg_on ^= 1; pm_fg_reload = 1; break;
                        case PM_OBJ: obj_wide ^= 1; break;
                        case PM_STATS: pm_stats ^= 1; show_hud = pm_stats; break;
                        case PM_EXIT: {                                      /* exit to the map */
                            uint32_t lv = game_wram16(0x563) & 0xFF;
                            if (lv == 0xEA) lv = 1;
                            extern uint8_t wram_lo[];
                            if (!pm_exit_addr) { pm_exit_addr = 0x583 + lv; pm_exit_orig = wram_lo[pm_exit_addr]; }
                            wram_lo[pm_exit_addr] |= 1;                   /* (as if cleared: Select exits) */
                            g = SNES_SELECT;
                            pm_exit_wait = 600;
                            break;
                        }
                        case PM_RIPPLE: rip_on ^= 1; pm_fg_reload = 1; break;   /* (reload, as FG) */
                        case PM_LEVELS: go_menu = 1; break;                  /* our level menu */
                        }
                    }
                    if ((press & PAD_START) && !pm_hold) g = SNES_START;
                    if (g) pm_req = g;
                    sp = 0;               /* (I12: see the step loop) */
                }
                if (pm_exit_addr && (--pm_exit_wait <= 0 || game_wai_pc() == 0x80E90E)) {
                    extern uint8_t wram_lo[];                             /* put the cleared flag back */
                    if (!(pm_exit_orig & 1)) wram_lo[pm_exit_addr] &= ~1;
                    pm_exit_addr = 0;
                }
            }
#ifdef GOTEST
            {   /* (test: no lives left - the first death is GAME OVER) */
                extern uint8_t wram_lo[];
                if (game_in_main_loop()) { wram_lo[0x575] = 0xFF; wram_lo[0x576] = 0xFF; }
            }
#endif
            {   /* (I21) OBJECT RANGE: the windows follow the view offset d (view = camx - d .. + 320):
                   camx - d - 32 .. camx - d + 352 (SNES: camx - 32 .. camx + 288) */
                extern uint32_t r65_ws[3];
                extern int32_t spr_xoff;
                int32_t L = spr_xoff < 0 ? 0 : spr_xoff > 64 ? 64 : spr_xoff;
                if (!obj_wide || !in_level) L = 0;
                r65_ws[0] = (uint32_t)L; r65_ws[1] = (obj_wide && in_level) ? 64 : 0; r65_ws[2] = r65_ws[1] - (uint32_t)L;
            }
            for (int st = 0; st < fsteps; st++) {
                if (st && (game_fblank || !game_in_main_loop()) && !(map_mode == 1 && game_wai_pc() == 0x80E90E) &&
                    !(map_mode == 2 && map_cab >= 0 && !game_dark()) &&     /* (I14: cabins - Funky's barrel ran slow) */
                    !(map_mode == 4 && !game_dark() && fe_same())) break;
                sp = snes_pad(pad_take(fsteps - st));     /* (I17: this logic frame's vblank sample) */
                if (start_block) {        /* (I12) Start picked the level in our menu: held on, the game
                                             paused at once and the menu would not close */
                    if (sp & SNES_START) sp &= ~SNES_START; else start_block = 0;
                }
                if (pm_open) {
                    /* (I12) paused: the game takes Start / Select only after 16 frames without them
                       (CODE_809AFF, $1E33) - every press before that started the wait again (the menu
                       "did not respond", mashing kept it open).  The menu's request goes in once the
                       wait is over; nothing else reaches the game meanwhile. */
                    sp = 0;
                    if (pm_req && (game_wram16(0x1E33) & 0x8000)) { sp = pm_req; pm_req = 0; }
                }
#ifdef GAMEDEMO
                {   /* test builds: run right, jump now and then */
                    static uint32_t f;
                    f++;
                    sp = SNES_RIGHT | SNES_Y;
                    if ((f % 90) < 12) sp |= SNES_B;
                    if ((f % 600) > 560) sp = SNES_LEFT;
                }
#endif
#ifdef GAMEBOT
                {   /* test builds: run right; stuck (DK x unchanged ~0.5 s) -> up, then down, then left */
                    static uint32_t f, lastx, still, mode, mt;
                    extern uint8_t wram_lo[];
                    uint32_t x = wram_lo[0xB1B] | wram_lo[0xB1C] << 8;
                    f++;
                    if (x == lastx) still++; else { still = 0; lastx = x; }
                    if (mt) mt--; else mode = 0;
                    if (still > 30 && !mt) { mode = 1 + (f / 37) % 3; mt = 60; still = 0; }
                    sp = SNES_RIGHT;
                    if ((f % 50) < 6) sp |= SNES_Y;
                    if (mode == 1) sp = SNES_UP | SNES_RIGHT | ((f & 8) ? SNES_B : 0);
                    else if (mode == 2) sp = SNES_DOWN | SNES_RIGHT;
                    else if (mode == 3) sp = (mt > 30 ? SNES_LEFT : SNES_UP) | ((f & 8) ? SNES_B : 0);
                    if (!(in_level && map_mode == 0 && game_in_main_loop())) {
                        /* (I13 test) not in a level (title, file select, maps): Start now and then, walk
                           right between (the next level on a map) */
                        uint32_t t = f % 90;
                        sp = t < 4 ? SNES_START : t >= 30 && t < 60 ? SNES_RIGHT : 0;
                    }
#ifdef BOTPRE
                    {   /* (test, Torchlight) walk to x BOTPRE, jump (Squawks' crate), wait, then the bot */
                        static uint32_t st, t;
                        if (!(in_level && map_mode == 0 && game_in_main_loop() && !game_dark())) ;
                        else if (st == 0) { sp = SNES_RIGHT; still = 0; if (x >= BOTPRE) { st = 1; t = 0; } }
                        else if (st == 1) { t++; sp = t < 12 ? SNES_RIGHT | SNES_B : t < 32 ? SNES_RIGHT : 0; still = 0;
                                            if (t >= 130) st = 2; }
                    }
#endif
                }
#endif
#ifdef GAMEBOSS
                {   /* test builds: walk to the boss (sprite id GAMEBOSS), jump onto it now and then */
                    static uint32_t f;
                    extern uint8_t wram_lo[];
                    f++;
                    sp = 0;
                    for (int k = 2; k < 48; k++)
                        if ((wram_lo[0xD45 + 2 * k] | wram_lo[0xD46 + 2 * k] << 8) == GAMEBOSS) {
                            int32_t bx = wram_lo[0xB19 + 2 * k] | wram_lo[0xB1A + 2 * k] << 8;
#ifdef GAMEBOSSHP
                            {   /* (test) the boss's hit points ($14F9,x) down to GAMEBOSSHP once */
                                static int hpdone;
                                if (!hpdone && wram_lo[0x14F9 + 2 * k] > GAMEBOSSHP) { wram_lo[0x14F9 + 2 * k] = GAMEBOSSHP; hpdone = 1; }
                            }
#endif
                            int32_t dx = bx - (int32_t)(wram_lo[0xB1B] | wram_lo[0xB1C] << 8);
                            sp = 0;
                            if (dx < 72 && dx > -72) sp = SNES_B | (dx > 8 ? SNES_RIGHT : dx < -8 ? SNES_LEFT : 0);
                            if ((f % 30) > 24) sp &= ~SNES_B;
                            break;
                        }
                    if (!sp && (f % 120) < 60) sp = SNES_RIGHT;
                }
#endif
#ifdef DBG_FREE
                {   /* (test) DBG_FREE=frames,dx: the game's free movement, right + up, DK x += dx every
                       4 frames until that game frame */
                    static const int32_t df[] = { DBG_FREE };
                    extern uint8_t wram_lo[];
                    if (game_in_main_loop() && !game_fblank && game_stats.frames < (uint32_t)df[0]) {
                        wram_lo[0x535] = 1; wram_lo[0x536] = 0;
                        sp = SNES_RIGHT | SNES_UP;
                        if (!(game_stats.frames & 3)) {
                            uint32_t x = wram_lo[0xB1B] | wram_lo[0xB1C] << 8; x += df[1];
                            wram_lo[0xB1B] = x & 255; wram_lo[0xB1C] = x >> 8;
                        }
                    } else if (game_stats.frames == (uint32_t)df[0]) wram_lo[0x535] = 0;
                }
#endif
#if defined(ENDTEST) && !defined(BOSSTEST)
                if (game_stats.frames == 30) { extern void game_debug_jump(uint32_t); game_debug_jump(0x00B884D7); }
#endif
#ifdef DBG_GOTO
                {   /* (test) DBG_GOTO=f1,ent1,f2,ent2: load these entrances at these game frames */
                    static const uint32_t gt[] = { DBG_GOTO };
                    for (unsigned k = 0; k + 1 < sizeof gt / sizeof gt[0]; k += 2)
                        if (game_stats.frames == gt[k]) {
                            extern uint8_t wram_lo[]; extern void game_debug_jump(uint32_t);
                            static const uint16_t w[4] = { 0x3E, 0x40, 0x2E, 0x565 };
                            for (int i = 0; i < 4; i++) { wram_lo[w[i]] = gt[k + 1] & 255; wram_lo[w[i] + 1] = gt[k + 1] >> 8; }
                            game_debug_jump(0x0080806F);
                        }
                }
#endif
#ifdef DBG_PAD
                {   /* (test) DBG_PAD=f0,f1,snes_bits,...: the SNES pad for game frames [f0, f1) */
                    static const uint32_t dp[] = { DBG_PAD };
                    for (unsigned k = 0; k + 2 < sizeof dp / sizeof dp[0]; k += 3)
                        if (game_stats.frames >= dp[k] && game_stats.frames < dp[k + 1]) sp = dp[k + 2];
                }
#endif
#ifdef DBG_TP
                {   /* (test, as sim3 TP=) DBG_TP=start,every,dx: the Kong's x += dx every `every` game frames */
                    static const int32_t tp[] = { DBG_TP };
                    extern uint8_t wram_lo[];
                    uint32_t f = game_stats.frames;
                    uint32_t tpend = sizeof tp / sizeof tp[0] > 3 ? (uint32_t)tp[sizeof tp / sizeof tp[0] - 1] : 0xFFFFFFFFu;  /* (optional 4th: end frame) */
                    if (f >= (uint32_t)tp[0] && f < tpend && (f - tp[0]) % tp[1] == 0) {
                        uint32_t x = wram_lo[0xB1B] | wram_lo[0xB1C] << 8; x += tp[2];
                        wram_lo[0xB1B] = x & 255; wram_lo[0xB1C] = x >> 8;
                    }
                }
#endif
#ifdef DBG_POKER
                {   /* (test) DBG_POKER=f0,f1,addr,val16,...: WRAM poke every game frame in [f0, f1) */
                    static const uint32_t pk[] = { DBG_POKER };
                    extern uint8_t wram_lo[];
                    for (unsigned k = 0; k + 3 < sizeof pk / sizeof pk[0]; k += 4)
                        if (game_stats.frames >= pk[k] && game_stats.frames < pk[k + 1]) { wram_lo[pk[k + 2]] = pk[k + 3] & 255; wram_lo[pk[k + 2] + 1] = pk[k + 3] >> 8; }
                }
#endif
#ifdef DBG_POKE
                {   /* (test) DBG_POKE=f,addr,val16,...: WRAM poke before that game frame */
                    static const uint32_t pk[] = { DBG_POKE };
                    extern uint8_t wram_lo[];
                    for (unsigned k = 0; k + 2 < sizeof pk / sizeof pk[0]; k += 3)
                        if (game_stats.frames == pk[k]) { wram_lo[pk[k + 1]] = pk[k + 2] & 255; wram_lo[pk[k + 1] + 1] = pk[k + 2] >> 8; }
                }
#endif
                game_frame(sp);
#ifdef DBGRING
                {   /* (test) per game frame: frames, $1C | $3E << 16, WAI pc, $563 | $565 << 16, last key, pad */
                    static volatile uint32_t dbg_ring[32][6] __attribute__((used));
                    static uint32_t dbg_ri __attribute__((used)), dbg_last[3];
                    extern volatile uint32_t r65_dbg_key;
                    uint32_t a = game_wram16(0x1C) | game_wram16(0x3E) << 16, b = game_wai_pc(), c = game_wram16(0x563) | game_wram16(0x565) << 16;
                    if (game_stats.frames > 1800 && (a != dbg_last[0] || b != dbg_last[1] || c != dbg_last[2])) {
                        dbg_last[0] = a; dbg_last[1] = b; dbg_last[2] = c;
                        volatile uint32_t *r = dbg_ring[dbg_ri++ & 31];
                        r[0] = game_stats.frames; r[1] = a; r[2] = b; r[3] = c; r[4] = r65_dbg_key; r[5] = sp | st << 24 | map_mode << 28;
                    }
                }
#endif
                if (game_stats.err) hang_show();      /* the game code failed: show where */
#ifdef DBG_HANG
                if (game_stats.frames == 400) for (dbg_stage = 0x99;;) ;    /* (test the watchdog) */
#endif
                fs_done = st + 1;
            }
            music_follow();
            pipe_ticks = 0;
            uint32_t pipe_logic = frt_since(g0, gv0);         /* (60 FPS mode estimate: the logic alone) */
            PERF_ADD(0, pipe_logic);
            PERF_MARK(-1);
#ifdef PERFREC
            {   extern uint32_t perf_lost[4]; perf_lost[2] += (uint32_t)(fsteps - fs_done); }
#endif
            if (af_pend) {                /* (I17) the last picture's swap, before any drawing */
                uint16_t w0 = frt_read(); uint32_t wv0 = vcount;
                fb_sync();
                af_wait += frt_since(w0, wv0);
                fb_wait_ticks = frt_since(w0, wv0);
                PERF_MARK(-1);
            }
            pipe_async = 0;
            if (pipe_pend) {
                /* 60 FPS mode: the slave's picture of the previous game frame - show it now */
                int pipe_logic_vlate = vcount != (uint32_t)pipe_vk;
                uint16_t p0 = frt_read();
                uint32_t pv0 = vcount;
                dbg_stage = 9;
                {   /* help with the slave's sprite bands from the bottom (not the HUD's) - (I17) only in
                       the master's slack: a band is taken while it can be done before the picture's
                       deadline (the flip at the next vblank: present() must start before line 222), or
                       once that vblank is missed anyway.  Helping beyond it delayed the master's next
                       logic frame: pictures took 2 vblanks while the slave sat idle half the time. */
                    static int32_t band_est = 16 * 16;      /* (lines x 16: the master's band time, EMA) */
                    int32_t nb = pb_count(pipe_gy), bmin = 0;
                    while (bmin < nb && pb_edge(pipe_gy, bmin) < 20) bmin++;
#ifdef PERFREC
                    {   int32_t l0 = current_line(); perf_ln[0]++; perf_ln[1] += l0 < 0 ? 0 : l0;
                        perf_ln[8 + (l0 < 0 ? 0 : l0 >= 224 ? 7 : l0 / 32)]++;
                        if (vcount != perf_pv) perf_ln[5]++;          /* (the logic ended after the vblank) */
                        if (COMM(C_DONE) == token) { perf_ln[3]++; perf_ln[2] += l0 < 0 ? 0 : l0; } }
                    perf_seen = COMM(C_DONE) == token;
#endif
                    while (COMM(C_DONE) != token && *UNCACHED(&pb_go) != (int32_t)token)
                        ;
                    while (COMM(C_DONE) != token) {
                        int32_t ln = current_line();
                        if (ln < 222 - (band_est >> 4) - 4 || ln >= 222) {
                            int32_t b = *UNCACHED(&pb_low) - 1;
                            if (b < bmin || b < *UNCACHED(&pb_next)) break;
                            *UNCACHED(&pb_low) = b;
                            spr_draw_band(pb_edge(pipe_gy, b), pb_edge(pipe_gy, b + 1), pipe_bi, pipe_gx, pipe_gy);
                            int32_t d = current_line() - ln;
                            if (d < 0) d += 262;
                            if (d > 200) d = 200;
                            band_est += (d * 16 - band_est) >> 3;
                        }
                    }
                }
#ifdef PERFREC
                while (COMM(C_DONE) != token)
                    ;
                if (!perf_seen) { int32_t l1 = current_line(); perf_ln[2] += l1 < 0 ? 0 : l1; if (vcount != perf_pv || l1 >= 222) perf_ln[4]++; }
#endif
                PERF_ADD(1, frt_since(p0, pv0));
                uint16_t pw0 = frt_read(); uint32_t pwv0 = vcount; (void)pw0; (void)pwv0;
                LDST2(2); while (COMM(C_DONE) != token)
                    ;
                PERF_ADD(2, frt_since(pw0, pwv0));
#ifdef PIPE_SPLIT
                {   /* (I17 test) a BG split: the slave late -> the master takes more rows; the master's
                       logic late -> fewer.  Measured (JH, MiSTer model): the master's BG rows cost far
                       more than the slave's (its tile cache is cold in this mode) - 33 vs 41 pictures/s */
                    if (pipe_logic_vlate) { if ((pipe_spl += 16) > SCREEN_H) pipe_spl = SCREEN_H; }
                    else if (vcount != (uint32_t)pipe_vk || current_line() >= 222) { if ((pipe_spl -= 8) < SCREEN_H / 3) pipe_spl = SCREEN_H / 3; }
                }
#else
                (void)pipe_logic_vlate;
#endif
                sh->job = 0;
                {   /* = the wait / bands after this logic + the work between the last flip and it */
                    uint32_t since = vcount - pipe_v1 <= 4 ? frt_since(pipe_t1, pipe_v1) : 0;
                    pipe_rest = since > pipe_logic ? since - pipe_logic : 0;
                }
                pillar_draw();
#ifdef PERFREC
                {   int32_t lp = current_line(); perf_ln[6] += lp < 0 ? 0 : lp; if (lp >= 222 || vcount != perf_pv) perf_ln[7]++; }
#endif
                {   uint16_t q0 = frt_read(); uint32_t qv0 = vcount; (void)q0; (void)qv0;
                    /* (I17) the swap can go on while the master sets up the next picture and runs the next
                       logic: the slave's job 5 waits for it; paths that draw themselves sync first */
                    pipe_async = game_on && in_level && map_mode == 0 && !pm_open && !fe_vm && !game_fblank &&
                                 game_in_main_loop() && !go_menu && !pm_fg_reload && sh->cv == 1 && !ripple &&
                                 !dither_ripple && !rain_n && !sh->ovcap && !sh->bm_lvl && !sh->nov && !sh->glow &&
                                 !lamp_n && !comp && !fog3;
                    present2(pipe_cx, pipe_cy, &frame_no, pipe_async);
                    PERF_ADD(3, frt_since(q0, qv0)); }
#ifdef PERFREC
                perf_pv = vcount;
#endif
                PERF_MARK(-1);
                pipe_pend = 0;
                pipe_ticks = frt_since(p0, pv0);
                pipe_t1 = frt_read(); pipe_v1 = vcount;
                pipe_logic_last = pipe_logic;
            }
#ifdef PROFH
            prof_on = 0;
#endif
            dbg_stage = 2;
            {   /* the 32X shows 64 px more on the right: at the level's right end ($1B25 = the
                   game's last camera x, $1B23 the first) the view stops 64 px earlier and the
                   sprites move right instead (no empty columns past the level) */
                extern int32_t spr_xoff;
                uint32_t c = game_wram16(0x88B), mn = game_wram16(0x1B23), mx = game_wram16(0x1B25);
                const uint32_t lvl_mn = mn;          /* (I21) the level's own first camera x */
                {   /* the current camera block's range (game_hle.c: CODE_80C246 / CODE_B8A853) - (I18) only
                       a block narrower than the 32X view (Coral Capers' shafts: centred).  The view used
                       to stop at every block's ends too: in the water levels, whose camera blocks change
                       as you swim, it slid 3 px a frame towards each new limit on its own - the "jumpy"
                       camera.  The view is the game's spawn window (camx - 32 .. + 288), so showing those
                       32 columns past a block's end shows the level as it is there.  Sticky while the
                       camera stays inside the block (frames without the camera routine kept switching
                       to the level's range). */
                    extern volatile uint32_t hle_cam_xmin, hle_cam_xmax, hle_cam_frame;
                    static uint32_t lastf, nb_mn, nb_mx, nb_on, nb_cmn, nb_cmx, nb_stable;
                    if (hle_cam_frame != lastf) {
                        lastf = hle_cam_frame;
                        /* (I21) only a range that stays the same: in the water levels the clamp range
                           moves every frame while the camera chases DK (xmin 884, 878, 883 .. 917 in the
                           user's Coral Capers state) - each frame's "narrow block" moved the view, 12 px
                           jumps.  A real shaft's range is fixed: 8 camera frames unchanged */
                        if (hle_cam_xmin == nb_cmn && hle_cam_xmax == nb_cmx) { if (nb_stable < 255) nb_stable++; }
                        else { nb_cmn = hle_cam_xmin; nb_cmx = hle_cam_xmax; nb_stable = 0; }
                        if (!nb_on && nb_stable >= 8) {
                            nb_mn = nb_cmn; nb_mx = nb_cmx;
                            nb_on = nb_mx < nb_mn + 64 && nb_mn >= mn && nb_mx <= mx;
                        }
                    }
                    if (nb_on && (c < nb_mn || c > nb_mx || game_fblank)) nb_on = 0;
                    if (nb_on) { mn = nb_mn; mx = nb_mx; }
                }
                uint32_t lim = mx >= mn + 64 ? mx - 64 : mn;
                /* (I16) the view is centred on the SNES picture: 32 columns each side.  DKC spawns
                   level objects from camx - $20 to camx + $120, exactly the 32X's 320 columns, so
                   nothing appears or vanishes inside the picture (the I11 widened spawn windows
                   changed the game logic - Barrel Cannon Canyon lost a barrel; I15 removed them and
                   objects popped in 32 px from the right edge).  At the level's ends the view
                   stops at the level: left, d shrinks to 0; right, d grows to 64. */
#ifndef WS_CENTER
#define WS_CENTER 32
#endif
                int32_t d = WS_CENTER;
                if (c < mn + WS_CENTER) d = c >= mn ? (int32_t)(c - mn) : 0;
                if (c > lim && (int32_t)(c - lim) > d) d = (int32_t)(c - lim);
                pillar_l = pillar_r = 0;
                if (mx < mn + 64 && c >= mn && c <= mx) {
                    /* a room narrower than the 32X view: centred.  A single-screen room (bonus rooms:
                       the whole level, $1B23-$1B25, is that narrow) gets black bars on both sides
                       instead of the map beyond it; a narrow camera block inside a level (Coral
                       Capers' shafts, H13) shows the level's map round it - black bars there flashed
                       on and off as the camera went in and out of the block */
                    int32_t w = (int32_t)(mx + 256 - mn), pl = (320 - w) >> 1;
                    uint32_t l0 = game_wram16(0x1B23), l1 = game_wram16(0x1B25);
                    if (l1 < l0 + 64) { pillar_l = pl; pillar_r = 320 - w - pl; }
                    d = (int32_t)(c - mn) + pl;
                }
                if (d > 64 || game_fblank || !game_in_main_loop()) { d = 0; pillar_l = pillar_r = 0; }
                int pill_new = 0;
                {   /* (H16) the bars are drawn into the canvas, whose pixels stay valid from frame to
                       frame: bars that appear, go or change width (the boss arena locks the camera
                       when the boss is beaten) left black in the picture ("black staircases", more
                       with the view easing past them).  A new bar state: both buffers redrawn and
                       the view jumps to its place */
                    static int32_t ppl, ppr;
                    if (pillar_l != ppl || pillar_r != ppr) {
                        ppl = pillar_l; ppr = pillar_r; pill_new = 1;
                        cvb[0].valid = cvb[1].valid = 0;
                    }
                }
                {   /* (eased: a new camera block can move the limit by up to 64 px at once) */
                    static int32_t dcur, was_off = 1;
                    if (was_off || pill_new || pillar_l) dcur = d;
                    else if (d > dcur + 3) dcur += 3;
                    else if (d < dcur - 3) dcur -= 3;
                    else dcur = d;
                    was_off = game_fblank || !game_in_main_loop();
                    d = dcur;
                }
                {   /* (G3) never past the level's built right end: the easing above must not show
                       the blank columns beyond it (level exits: the limit arrives late) */
                    int32_t over = (int32_t)c - (int32_t)cur_lv->xmax;
                    if (!game_fblank && game_in_main_loop() && over > d && over <= 64 && !pillar_l) d = over;
                    /* (I16) nor left of the level's first camera x (the easing lags while the
                       camera goes back to the level's start) */
                    /* (I21: the LEVEL's start, not a narrow camera block's - inside Coral Capers' shafts
                       this clamp cut the eased d at once: the view jumped 12 px entering / leaving them) */
                    if (!pillar_l && c >= lvl_mn && d > (int32_t)(c - lvl_mn) && d <= WS_CENTER) d = (int32_t)(c - lvl_mn);
                }
                spr_xoff = d;
#ifdef CAMLOG
                {   /* (test) the last 128 pictures: c, block range used, d, cam frames - read from an SDRAM dump */
                    static volatile uint32_t cam_log[128][4] __attribute__((used));
                    static uint32_t cam_li __attribute__((used));
                    volatile uint32_t *r = cam_log[cam_li++ & 127];
                    r[0] = c | (uint32_t)game_wram16(0x895) << 16; r[1] = mn | mx << 16; r[2] = (uint32_t)d | game_stats.frames << 8;
                    { extern volatile uint32_t hle_cam_frame, hle_cam_xmin, hle_cam_xmax; r[3] = hle_cam_frame; }
                }
#endif
#ifdef CAMRING
                {   /* (test) per picture: c, $1B23, $1B25, block min / max, d, pillars, game frames */
                    static volatile uint32_t cam_ring[16][8] __attribute__((used));
                    static uint32_t cam_ri __attribute__((used));
                    extern volatile uint32_t hle_cam_xmin, hle_cam_xmax, hle_cam_frame;
                    volatile uint32_t *r = cam_ring[cam_ri++ & 15];
                    r[0] = c; r[1] = game_wram16(0x1B23) | game_wram16(0x1B25) << 16; r[2] = hle_cam_xmin | hle_cam_xmax << 16;
                    r[3] = mn | mx << 16; r[4] = (uint32_t)d; r[5] = pillar_l | pillar_r << 16; r[6] = hle_cam_frame; r[7] = game_stats.frames;
                }
#endif
            }
            PERF_MARK(5);
            {   uint16_t sp0 = frt_read(); uint32_t spv0 = vcount; (void)sp0; (void)spv0;
                /* (I17) 60 FPS mode: after the kick (the slave starts its BG earlier; its sprite bands
                   wait for pipe_mbg); every other path runs it here or before its render (spr_defer) */
                if (PIPE_SPRDEFER && pipe_mode && pipe_now && game_on && in_level && !game_fblank && game_in_main_loop() && !pm_open)
                    spr_defer = 1;
                else
                    spr_prepare();
                PERF_ADD(4, frt_since(sp0, spv0)); }
            PERF_MARK(-1);
            if (ms_blk) {   /* the millstone: CODE_809278 (BG2 HOFS = camx + $80 - $DD, VOFS = $DF - $1C0 + camy;
                               shown above the SNES view too: the 32X's extra 64 columns) */
                extern volatile int32_t spr_ms_on, spr_ms_x0, spr_ms_y0;
                extern const uint16_t *volatile spr_ms_cells;
                extern int32_t spr_xoff;
                uint32_t dd = game_wram16(0xDD), f = game_wram16(0x28);
                int32_t a = (int16_t)(game_wram16(0x88B) + 0x80 - dd);
                int32_t v = (int16_t)(game_wram16(0xDF) - 0x1C0 + game_wram16(0x895));
                int on = game_on && dd != 0xFE00 && a < 0xC0 && -a + spr_xoff < SCREEN_W && v < 0x80 && v >= -0xE0;
                if (on) {
                    uint32_t t = (f & 3) == 3 ? f : (f & ~3u) - 1;   /* the last tilemap DMA (frames with $28 & 3 = 3) */
                    extern volatile int32_t spr_ms_k;
                    spr_ms_k = (~t >> 2) & 3;
                    spr_ms_cells = ms_cells0 + spr_ms_k * 448;
                    spr_ms_x0 = -a + spr_xoff;
                    spr_ms_y0 = -v - 1;
                }
#ifdef MSTEST
                on = 1; spr_ms_x0 = 60; spr_ms_y0 = 30;   /* (test: the picture always on, top left) */
                { extern volatile int32_t spr_ms_k; spr_ms_k = (f >> 2) & 3; spr_ms_cells = ms_cells0 + spr_ms_k * 448; }
#endif
                spr_ms_on = on;
            }
#ifdef GAMEPROF
            *(volatile uint16_t *)&ares_phase = 0;
#endif
            if (pm_frz && pm_open && pm_key == PM_KEY() && !go_menu) {   /* (I13: LEVEL SELECT must get through) */
                while (REG16(VDP_FBCTL) & VDP_VBLK)
                    ;
                while (!(REG16(VDP_FBCTL) & VDP_VBLK))
                    ;
                continue;
            }
            dbg_stage = 3;
            game_ticks = frt_since(g0, gv0);
            game_ticks = game_ticks > pipe_ticks + fb_wait_ticks ? game_ticks - pipe_ticks - fb_wait_ticks : 0;
            PT(2);
            /* (I17) the early job's time checks (HUD restore, overlay rows) moved to where the master
               waits for that job anyway (below, before the BG): the wait here cost ~3 % of a frame when
               the job ran past the logic, with the sprite setup still to do meanwhile */
            j3_logic = early ? (int32_t)game_ticks : -1;
            if (early)                    /* (the slave has copied job 3's fields: sh may change now) */
                while (COMM(C_DONE) != token && *UNCACHED(&s_job_now) != 0x103)
                    ;
            PERF_MARK(16);
            PT(3);
#ifdef PROFH
            if (game_stats.frames == 60) {
                volatile uint32_t *vt = (volatile uint32_t *)0x06000000;
                prof_base = (uint32_t)gtext_start;
                for (int i = 0; i < 0x48; i++) if (vt[i] == (uint32_t)master_hint) vt[i] = (uint32_t)prof_hint;
            }
            if (game_stats.frames == 360) prof_show();
#endif
#ifdef GAMEDEMO
            {   /* average over game frames 60..359 (shown once done) */
                static uint32_t acc, n;
                if (game_stats.frames >= 60 && game_stats.frames < 360) { acc += game_ticks; n++; }
                if (game_stats.frames >= 360 && n) game_avg = acc * 100 / n / FRAME_TICKS;
            }
#endif
            if (pm_fg_reload && !game_fblank) {     /* foreground on / off: rebuild the level's renderer */
                pm_fg_reload = 0;
                if (early) { LDST2(5); while (COMM(C_DONE) != token) ; sh->job = 0; early = 0; }
                game_reload = 1;
                load_level(cur_idx, sh);
                game_reload = 0;
            }
            pcamx = camx; pcamy = camy;
            {   extern int32_t spr_xoff; camx = (int32_t)game_wram16(0x88B) - spr_xoff; }   /* (unsigned: rooms sit at x > 32767) */
            camy = (int16_t)game_wram16(0x895) + 1;
            PERF_MARK(17);
            if (game_fblank && early) {
                LDST2(6); while (COMM(C_DONE) != token)
                    ;
                sh->job = 0;
                early = 0;
            }
            if (game_fblank && game_wai_pc() == 0x80CF2B && !full_game)
                go_menu = 1;                      /* the title screen (after GAME OVER): back to the level menu */
            else if (game_fblank && game_wai_pc() == 0x80DF7C && !game_dark() && !(full_game && hdr->off_fe)) {
                /* the GAME OVER screen (its picture is not in this build): the words on black */
                if (map_mode != 3) { map_mode = 3; go_dirty = 2; CRAM[254] = 0x8000; CRAM[255] = 0xFFFF; blanked = 0; }
                if (go_dirty) {
                    go_dirty--;
                    write_line_table(0, 0);
                    box(0, 0, SCREEN_W, SCREEN_H, 254);
                    text(160 - text_width("GAME OVER") / 2, 104, "GAME OVER", 255);
                }
                spr_pal_dirty = 0; spr_bgpal_dirty = 0; pending_pal = 0;   /* (the game's palettes: not shown) */
                present(0, 0, &frame_no);
                CRAM[254] = 0x8000; CRAM[255] = 0xFFFF; CRAM[0] = 0x8000;    /* (vblank: right after the flip) */
                for (int i = 0; i < SCREEN_H; i++) *UNCACHED(&sky_ram[i]) = 0x8000;
                continue;
            }
#ifdef MUSRESET
            if (game_stats.frames == MUSRESET) {   /* (test: music refill counters from here on) */
                extern volatile uint32_t music_stat_rst;
                *UNCACHED(&music_stat_rst) = 1;     /* (the slave's music interrupt clears them) */
            }
#endif
#ifdef SRAMPOKE
            if (game_stats.frames == SRAMPOKE) { game_sram()[100] ^= 0x5A; game_sram_dirty = 1; }   /* (test: new save data) */
#endif
            if (game_fblank && !go_menu && game_sram_dirty && full_game) {
                /* new save data (Candy's, the file select): to the cartridge once the game has finished
                   writing (no level is drawn: the slave is idle) */
                static uint32_t sd_f;
                if (game_sram_dirty == 1) { game_sram_dirty = 2; sd_f = game_stats.frames; }
                else if (game_stats.frames - sd_f > 20) sram_save();
            }
            if (game_fblank && !go_menu) {
                /* the game loads with the screen off and uses load-only WRAM pages that alias the tile
                   cache: keep showing the last frame, then rebuild the level graphics
                   (TODO: show black / fade - INIDISP brightness is not emulated yet) */
                if (!game_dark() && !game_in_main_loop()) {
                    /* the game's map screen (WAI 80E90E); other screens (Cranky's cabin, Funky's...)
                       have no picture in this build: their sprites on black */
                    int want = game_wai_pc() == 0x80E90E ? 1 : 2;
                    /* the map screen the game shows ($02, CODE_80E225): its own picture if the build has it */
                    int mi = want == 1 && hdr->off_maps ? (int)(game_wram16(0x02) & 0xFF) : -1;
                    if (mi > 12 || (mi >= 0 && !PK(mapscr_t, hdr->off_maps)[mi].off_img)) mi = -1;
                    /* a cabin ($3E = EE-F3 Cranky, F4-F9 Funky, FA-FF Candy) */
                    uint32_t e3 = game_wram16(0x3E) & 0xFF;
                    int cab = want == 2 && hdr->off_screens && e3 >= 0xEE ? (int)(e3 - 0xEE) / 6 : -1;
                    fe_lend();
                    {   /* a front-end screen (title, file select...): drawn live (fescr.c) */
                        /* (H15: also from a cabin - Candy's barrel shows the save-slot screen, NMI F894
                           like the file select; the cabins' own NMI F142 is no front-end screen) */
                        int fe = want == 2 ? fe_find() : -1;
                        if (fe >= 0) {
                            if (map_mode != 4 || fe != fe_idx) { fe_setup(fe); map_mode = 4; }
                            blanked = 0;
                            PATHLOG(1, map_mode | fe_idx << 8);
                            fe_frame(&frame_no);
                            continue;
                        }
                    }
                    if (want == 2 && cab < 0 && full_game) {
                        /* (I13) a screen this build has no picture for (the game's NMI between two map
                           screens after a level, ~6 frames: the map's path sprites over the backdrop,
                           smeared - "flashing graphics between maps / stages"): black, as while the
                           screen is off */
                        while (REG16(VDP_FBCTL) & VDP_VBLK)
                            ;
                        while (!(REG16(VDP_FBCTL) & VDP_VBLK))
                            ;
                        if (!blanked) {
                            PATHLOG(3, map_mode | fe_idx << 8);
                            blanked = 1;
                            map_mode = 0;
                            for (int i = 0; i < SCREEN_H; i++) *UNCACHED(&sky_ram[i]) = 0x8000;
                            for (int i = 0; i < 256; i++) CRAM[i] = 0x8000;
                        }
                        continue;
                    }
                    if (map_mode != want || (want == 1 && mi != map_mi) || (want == 2 && cab != map_cab)) {
                        map_black = want == 2 && cab < 0;
                        int w = 0;
                        for (int k = 0; k < (int)hdr->nworld; k++) {
                            const world_t *wd = PK(world_t, hdr->off_worlds) + k;
                            for (uint32_t j = 0; j < wd->nlev; j++) if (wd->idx[j] == cur_idx) w = k;
                        }
                        map_setup(cab >= 0 ? -100 - cab : mi >= 0 ? -1 - mi : w);
                        map_mode = want;
                        blanked = 0;
                    }
                    PATHLOG(2, map_mode | fe_idx << 8);
                    map_frame();
#ifdef MAPDBG
                    if (map_black) {   /* (test) which game state shows sprites on black */
                        char b[40]; hexs(b, game_wai_pc(), 6); b[6] = ' '; hexs(b + 7, game_wram16(0x02), 4); b[11] = ' ';
                        hexs(b + 12, game_wram16(0x3E), 4); b[16] = 32; hexs(b + 17, game_wram16(0x1C), 4); b[21] = 0;
                        CRAM[254] = 0x8000; CRAM[255] = 0xFFFF;
                        write_line_table(0, 0);
                        box(40, 200, 140, 12, 254); text(42, 202, b, 255);
                    }
#endif
                    present(0, 0, &frame_no);
                    continue;
                }
                if (game_dark() || !game_in_main_loop()) {
                    while (REG16(VDP_FBCTL) & VDP_VBLK)
                        ;
                    while (!(REG16(VDP_FBCTL) & VDP_VBLK))
                        ;
                    fe_lend();                    /* (the slave is idle: the next screen's maps can be mirrored) */
                    fe_palsum = 0xFFFFFFFF;
                    if (full_game) {
                        /* the game loads the entrance $3E now (its level data, entrance tables): that
                           world's bank in window 6 already - the ending goes Gang-Plank Galleon (world
                           7) -> the hoard (Kongo Jungle) -> the credits (drawn without a level, with
                           world 7's bank).  (The map set it for levels entered from the map.) */
                        uint32_t ent = game_wram16(0x3E) & 0xFF, b6 = fe_ent_bank6(ent);
                        if (!b6 && hdr->off_rooms) {
                            uint32_t r = PK(uint8_t, hdr->off_rooms)[ent];
                            if (r < hdr->nlev) b6 = LV_BANK6(PK(level_t, hdr->off_levels) + r);
                        }
                        if (b6) set_bank6(b6);
                    }
                    if (!blanked) {
                        /* the screen is off: black (in front of the Genesis planes), not the last picture
                           (whose colours the next screen's palette would scramble) */
                        PATHLOG(3, map_mode | fe_idx << 8);
                        blanked = 1;
                        map_mode = 0;             /* (a map screen after this sets its palette up again) */
                        for (int i = 0; i < SCREEN_H; i++) *UNCACHED(&sky_ram[i]) = 0x8000;
                        for (int i = 0; i < 256; i++) CRAM[i] = 0x8000;
                    }
                    continue;
                }
                if (full_game && hdr->off_fe) {
                    /* a main-loop screen that is no level (the credits: DK's treehouse with the cast as
                       sprites): drawn by fescr.c, keyed by the entrance; game_fblank stays set */
                    fe_lend();
                    int fe = fe_find();
                    if (fe >= 0) {
                        if (map_mode != 4 || fe != fe_idx) {
                            uint32_t b6 = fe_ent_bank6(game_wram16(0x3E) & 0xFF);
                            if (b6) set_bank6(b6);
                            fe_setup(fe);
                            map_mode = 4;
                        }
                        blanked = 0;
                        PATHLOG(4, map_mode | fe_idx << 8);
                        fe_frame(&frame_no);
                        continue;
                    }
                }
                map_mode = 0;
                game_fblank = 0;
                game_reload = 1;
                {   /* the level the game has loaded (entrance $3E), if this build has it */
                    int idx = cur_idx;
                    uint32_t ent = game_wram16(0x3E);
                    {   /* entrance -> room (pack level): the level, a bonus room, or (exits from bonus
                           rooms, checkpoints) the level they lead into */
                        uint32_t r = hdr->off_rooms && ent < 256 ? PK(uint8_t, hdr->off_rooms)[ent] : 0xFF;
                        if (r < hdr->nlev) idx = (int)r;
                    }
                    PATHLOG(5, map_mode | fe_idx << 8);
                    load_level(idx, sh);
                }
                game_reload = 0;
                blanked = 0;
            }
        }
        /* (I14) A + Start = our level menu only in the level viewer: in the full game it threw the
           player out of a level into the menu (Start there restarted Jungle Hijinxs: progress lost) -
           LEVEL SELECT in the options menu is the way there */
        if (go_menu || ((press & PAD_START) && (!game_on || ((pad & PAD_A) && !full_game)))) {
            go_menu = 0;
            map_mode = 0;
            render_state_reset();
            COMM(C_CMD) = 0x8000 | 0x7FFF;
            while (COMM(C_CMD) & 0x8000)
                ;
            for (int i = 0; i < SCREEN_H; i++) sky_ram[i] = 0;
            in_level = 0;
            *UNCACHED(&music_light) = 0;
            ripple = 0;
            comp = 0;
            comp_fog = 0;
            comp_pal_dirty = 0;
            CCR = 0x10;                   /* back to 4-way cache */
            CCR = 0x11;
            REG8(SYS_INTMASK) = 0x0C;
            sft_show = 0;
            sh->menu = 1;
            dirty = 2;
            set_bank(worlds[world].bank); pending_pal = PK(uint16_t, worlds[world].off_pal);
            pending_sky = 1;
            /* (I13) black until the menu's first picture (the level's picture in the menu's colours) */
            for (int i = 0; i < SCREEN_H; i++) *UNCACHED(&sky_ram[i]) = 0x8000;
            for (int i = 0; i < 256; i++) CRAM[i] = 0x8000;
            music_play(music_on ? menu_song() : 0xFF, 0);
            continue;
        }
        if ((press & PAD_A) && !game_on) {
            if (pad & PAD_B) {                /* B + A: music on/off */
                music_mode = (music_mode + 1) % 3;
                *UNCACHED(&music_light) = music_mode == 1;
                if (music_mode != 1)               /* auto <-> light switch on the fly */
                    music_play(music_on ? level_song(cur_lv) : 0xFF, comp);
                music_msg = 120;
            } else {
                show_hud ^= 1;
            }
        }
        if ((press & PAD_C) && (pad & PAD_B) && (comp || sh->cv == 2) && !game_on) {
            /* B + C in the fog levels: performance (fog behind the playfield) / fidelity */
            comp_perf ^= 1;
            int32_t kx = camx, ky = camy;
            load_level(cur_idx, sh);
            camx = kx; camy = ky;
            continue;
        }
        if ((press & PAD_C) && !(pad & PAD_B) && !game_on) autoscroll ^= 1;
        int spd = (pad & PAD_B) ? 8 : 3;
        if (!game_on) {
            if (pad & PAD_LEFT)  camx -= spd;
            if (pad & PAD_RIGHT) camx += spd;
            if (pad & PAD_UP)    camy -= spd;
            if (pad & PAD_DOWN)  camy += spd;
        }
        if (autoscroll && !game_on) {
#ifdef AUTOPATH
            {   /* test path: both directions, varying speeds, occasional jumps */
                static uint32_t f;
                f++;
                camx += ((f >> 6) & 3) < 2 ? 3 : -2;
                camy += ((f / 45) & 3) < 2 ? 2 : -3;
                if (f % 300 == 299) { camx += 150; camy -= 90; }
                if (f % 170 == 169) camx -= 13;
            }
#else
            camx += 2;
            if (camx > cur_lv->xmax) camx = cur_lv->xmin;
#endif
        }
        if (!game_on) {
            if (camx < cur_lv->xmin) camx = cur_lv->xmin;
            if (camx > cur_lv->xmax) camx = cur_lv->xmax;
            if (camy < cur_lv->ymin + 1) camy = cur_lv->ymin + 1;
            if (camy > cur_lv->ymax) camy = cur_lv->ymax;
        }

        sh->camx = camx;
        sh->camy = camy;
        sh->spr = game_on && (sh->cv == 1 || sh->cv == 2);   /* (2: the perf comp levels - Poison Pond) */
        PERF_MARK(18);
        LDST2(0x101);
        ws_frame(sh, camx);
        LDST2(0x102);
        if (sh->bm_lvl && sh->bm_game) {
            /* (I10) the game's own beam: its window table (Squawks' position and pose), per line;
               copied to BM_LN once the slave is idle (the canvas job below) */
            extern int32_t spr_xoff;
            int32_t ax, ay;
            sh->bm_kind = game_beam_lines(bm_lines, spr_xoff, SCREEN_H, &ax, &ay);
            sh->bm_on = 1;
            sh->bm_ax = ax; sh->bm_ay = ay;
        } else if (sh->bm_lvl) {
            /* Squawks' beam: on once his crate is passed; the apex bobs with the frame number this
               frame will be presented as */
            uint32_t t = ((uint32_t)(uint16_t)(frame_no + 1) >> 2) & 31;
            if (t >= 16) t = 31 - t;
            sh->bm_on = lt_state;
            sh->bm_ax = bm_ax0;
            sh->bm_ay = bm_ay0 + (int32_t)((t * 7) >> 4) - 3;
#ifdef NOBOB
            sh->bm_ay = bm_ay0;
#endif
        }
        if (sh->bm_lvl) {
            /* the 68000 moves its highlight sprites (BG2 lit in the beam) to the same apex */
            int32_t ax = sh->bm_ax < 0 ? 0 : sh->bm_ax > 126 ? 126 : sh->bm_ax;
            int32_t ay = sh->bm_ay < 0 ? 0 : sh->bm_ay > 0xFE ? 0xFE : sh->bm_ay;
            /* (I11: in the game the background stays dark - the user's call: it is far behind) */
            COMM(10) = (uint16_t)(0x4000 | (ax >> 1) << 8 | (sh->bm_on && !sh->bm_game ? ay : 0xFF));
        }
        anim_frame++;
        for (int k = 0; k < sh->nov; k++) {
            const ov_t *o = &ovinfo[k];
            if (o->xm == 4) {
                /* heavy snow (CODE_808B1F): screen-fixed, wind drift + sway, odd/even VOFS */
                int32_t f = anim_frame;
                int32_t t = f & 0x7F;
                if (t & 0x40) t ^= 0x7F;
                int32_t sw = snow_sway[t] >> 1;
                if (f & 0x80) sw = ~sw;
                sh->ovx[k] = (sw + f) & 0xFF;
                sh->ovy[k] = (f & 1) ^ 1;
                sh->wob_phase = (~(f >> 1)) & 31;
                sh->anim_cur[k] = (f & 0xE) >> 1;
            } else {
                sh->ovx[k] = scroll_val(camx, o->xm, o->xs, 0, 0xFFFF);
                sh->ovy[k] = scroll_val(camy - 1, o->ym, o->ys, o->ysub, o->yclamp);
            }
        }
        if (lamp_n)
            ;   /* (the cone + its flicker are on the Genesis; lamp_frame's CRAM 228-237 = sprite palette 6) */
        else if (sh->glow)
            sh->nov = glow_frame(sh);
#ifndef CAMX
        LDST2(0x103);
        if (ripple) ripple_phase++;
#endif
        sh->ripple_phase = ripple_phase;
        sh->rip = ripple && game_on;
        *UNCACHED(&spr_rip_pat) = (ripple && game_on && !pm_open) ? ripple_pat : 0;     /* (sprites.c; I18: flat with the menu) */
        *UNCACHED(&spr_rip_phase) = ripple_phase;
        if (comp) {
            if (1)
                ring_update(sh, (camx - sh->ox0 + 25) >> 3, (camy - sh->oy0) >> 3);
            if (comp_fog) {
                misty_frame(anim_frame, camx, camy - 1);
            } else {
                for (int i = 0; i < SCREEN_H / 4; i++) {
                    comp_wave[i][0] = sh->ovx[1];
                    comp_wave[i][1] = sh->ovy[1] + 1;
                }
            }
            comp_P = comp_wave[0][0] & 3;
        }
        LDST2(0x104);
        if (fog3)
            misty_frame(anim_frame, camx, camy - 1);
        if (early) {                      /* the slave's early restore must be finished */
            LDST2(7); while (COMM(C_DONE) != token)
                ;
            sh->job = 0;
            early = 0;
            if (j3_logic >= 0) {
                /* the HUD restore goes to the slave's early job while that stays within the logic time;
                   overlay levels: rows restored early adapt, the rest go to cv_band */
                int32_t j3 = *UNCACHED(&sh->j3_ticks);
                if (j3 > j3_logic) hud_early = 0;
                else if (j3 + 3000 < j3_logic) hud_early = 1;
                if (sh->ovcap) {
                    if (j3 > j3_logic && sh->ov_rows > 0) sh->ov_rows--;
                    else if (j3 * 5 < j3_logic * 4 && sh->ov_rows < 32) sh->ov_rows++;
                }
            }
            j3_logic = -1;
            if (sh->pdraw) {
                /* its predicted window is valid now - unless the real camera is more than a tile off */
                int bi = cur_fb ^ 1;
                int32_t gx = CV_GX(sh, camx), gy = CV_GY(sh, camy);
                int32_t nx = gx >> 3, ny = gy >> 3;
                cvb[bi].w = pwin;
                if (nx - pwin.x0 > 1 || pwin.x0 - nx > 1 || ny - pwin.y0 > 1 || pwin.y0 - ny > 1) cvb[bi].valid = 0;
                sh->pdraw = 0;
            }
        }
#if defined(GAMEPROF) && defined(PHASE_BG)
        *(volatile uint16_t *)&ares_phase = 1;          /* (ares profiler: BG phase) */
#endif
        int give = !sh->cv;               /* canvas mode: the slave helps with full redraws and overlays */
        int fused = 0;                    /* simple game levels: one band job per CPU (job 5) */
        int pipe = 0;                     /* 60 FPS mode this frame: the slave draws it all (job 5) */
        static int32_t f_spl = SCREEN_H / 2;
        static uint16_t spr_ticks0;
        trect_t fused_hud = { 0, 0, 0, 0 };
        int32_t mrows[2] = { 0, 0 };
        uint16_t tm0 = frt_read();
        if (af_pend && !(game_on && pipe_async && sh->cv == 1 && !rain_n))
            fb_sync();                    /* (I17: only the 60 FPS mode's job 5 waits for the swap itself) */
        if (sh->cv) {
            int bi = cur_fb ^ 1;
            int32_t gx = CV_GX(sh, camx), gy = CV_GY(sh, camy);
            trect_t N = { gx >> 3, (gx >> 3) + CV_TW, gy >> 3, (gy >> 3) + CV_TH };
            {   /* the millstone picture's canvas position this frame (cv_band: restores it covers) */
                extern volatile int32_t spr_ms_on, spr_ms_x0, spr_ms_y0;
                volatile int32_t *cq = UNCACHED(&spr_ms_cq[0]);
                extern volatile int32_t spr_ms_k;
                cq[0] = gx + spr_ms_x0; cq[1] = gy + spr_ms_y0; cq[2] = spr_ms_on; cq[3] = spr_ms_k;
            }
            if (sh->cv == 3) {
                int32_t mx = 0x7FFFFFFF;
                for (int i = 0; i < SCREEN_H / 4; i++) if (comp_wave[i][0] < mx) mx = comp_wave[i][0];
                if (mx - cv_xbase > 30000 || mx - cv_xbase < 256) {   /* keep the canvas inside the FB */
                    cv_xbase = ((mx - 1024) >> 8) << 8;
                    cvb[0].valid = cvb[1].valid = 0;
                    cvb[0].hud = cvb[1].hud = (trect_t){ 0, 0, 0, 0 };   /* (repainted: no old HUD text) */
                }
                cv3_rect(&N, 0, SCREEN_H / 4, 0, 321);
            }
            cvbuf_t *cb = &cvb[bi];
            trect_t I = { N.x0 > cb->w.x0 ? N.x0 : cb->w.x0, N.x1 < cb->w.x1 ? N.x1 : cb->w.x1,
                          N.y0 > cb->w.y0 ? N.y0 : cb->w.y0, N.y1 < cb->w.y1 ? N.y1 : cb->w.y1 };
            int full = !cb->valid || I.x0 >= I.x1 || I.y0 >= I.y1;
#ifdef PICPROF
            pp_full = full * 10 + (!cb->valid);
#endif
            if (rain_n) {
                if (full) rain_restore_all(bi, sh);   /* (otherwise in the rain phase) */
#ifdef RAINCHECK
                {   /* stale rain pixels left on this buffer after the restore */
                    uint32_t rows = 0, ly = 0;
                    for (int y = 0; y < SCREEN_H; y++) {
                        uint32_t a = (uint32_t)FB16[y] * 2, c = 0;
                        for (int x = 1; x < 319; x++) {
                            uint8_t v = FB8[a + x];
                            if (v == rain_ci[0] || v == rain_ci[1] || v == rain_ci[2]) c++;
                        }
                        if (c) { rows++; ly = y; }
                    }
                    rc_bad = ly / 4; rc_y = rows;

                }
#endif
            }
            /* (Torchlight stays out: tried in I12 - in PicoDrive the slave alone drawing the beam and the
               sprites was slower, ares no better) */
            fused = game_on && sh->cv == 1 && !ripple && !dither_ripple && !rain_n && !sh->ovcap && !sh->bm_lvl
                    && !sh->nov && !sh->glow && !lamp_n;
            {
                extern uint32_t *spr_ck;
#ifdef PIPE_FORCE
                pipe_off = 0;                     /* (test: never the automatic fallback) */
#endif
#ifdef PERFREC
                if (perf_cfg[3] & 2) pipe_off = 0;
#endif
                pipe = fused && pipe_mode && in_level && !pm_open && spr_ck && !pipe_off;
                if (pipe != pipe_now) { pipe_now = pipe; pv_sum = pv_n = 0; }
                if (!pipe) fb_sync();     /* (I17) */
            }
#ifndef FUSED
#ifdef PERFREC
            if (!pipe && !(perf_cfg[3] & 4)) fused = 0;
#else
            if (!pipe) fused = 0;         /* (ares: no gain - T 86 vs 85; the FB writes do not scale) */
#endif
#endif
            if (fused) {
                /* one job per CPU after the logic: its band's BG work (cv_band), then its sprites; the
                   slave takes the top band (+ the line table and the HUD), split on a canvas tile row */
                LDST2(8); while (COMM(C_DONE) != token)
                    ;
                int32_t split = ((gy + f_spl + 7) & ~7) - gy;
                int32_t ts = (gy + split) >> 3;
                if (ts < N.y0) ts = N.y0;
                if (ts > N.y1) ts = N.y1;
                if (pipe) {                     /* (the slave: every sprite; the BG rows above pipe_spl) */
                    split = SCREEN_H;
                    int32_t ml = ((gy + pipe_spl + 7) & ~7) - gy;
                    if (ml > SCREEN_H) ml = SCREEN_H;
                    ts = (gy + ml) >> 3;
                    if (ts < N.y0) ts = N.y0;
                    if (ts > N.y1) ts = N.y1;
                    if (ml >= SCREEN_H) ts = N.y1;
                    sh->pmline = ts >= N.y1 ? SCREEN_H : ml;
                }
                mrows[0] = ts; mrows[1] = N.y1;
                sh->frows[0] = N.y0; sh->frows[1] = ts;
                sh->sgx = gx; sh->sgy = gy; sh->sbi = bi; sh->ssplit = split;
                sh->ltx = gx; sh->lty = gy;
                cv_x0par = (gx + CV_XOFF) & 1;
                sh->hud_on = 0;
                if (show_hud && game_on) {
                    char *L = sh->hud;           /* (game levels: the short form, as the master's) */
                    L[0] = 'F'; fmt_num(L + 1, fps, 2); L[3] = ' ';
                    L[4] = 'M'; fmt_num(L + 5, m_pct, 2); L[7] = ' ';
                    L[8] = 'S'; fmt_num(L + 9, s_pct, 2); L[11] = ' ';
                    L[12] = 'G'; fmt_num(L + 13, m68_pct > 99 ? 99 : (int)m68_pct, 2); L[15] = 0;
                    sh->hud_on = 1;
                } else if (show_hud) {
                    char *L = sh->hud;
                    L[0] = 'F'; L[1] = 'P'; L[2] = 'S'; L[3] = ' ';
                    fmt_num(L + 4, fps, 2);
                    L[6] = ' '; L[7] = 'M'; L[8] = ' ';
                    fmt_num(L + 9, m_pct, 2);
                    L[11] = '%'; L[12] = ' '; L[13] = 'S'; L[14] = ' ';
                    fmt_num(L + 15, s_pct, 2);
                    L[17] = '%'; L[18] = 0;
                    sh->hud_on = 1;
                }
            }
            if (full || sh->ovcap || sh->bm_lvl) give = 1;
            if (fused) give = 0;
            if (give) {                   /* the slave must be idle before the job changes */
                LDST2(9); while (COMM(C_DONE) != token)
                    ;
            }
            if (sh->bm_game) {            /* (I10) this frame's beam edges, both CPUs read them */
                /* (I14) while Squawks turns, the beam sweeps across the screen for ~16 frames: nearly
                   every line's edges jump, and one picture redrew most of the screen in both buffers
                   (lit + dark tiles: 6-10 frames, the stutter turning round).  The drawn edges now
                   follow the game's at most BM_STEP pixels a picture (the open sides as x -/+ 400):
                   the beam slides round over a few pictures, each with a bounded redraw. */
#ifndef BM_STEP
#define BM_STEP 48
#endif
#ifdef NO_BMRATE
                for (int i = 0; i < 2 * SCREEN_H; i++) bm_shown[i] = bm_lines[i];
#else
                for (int i = 0; i < 2 * SCREEN_H; i++) {
                    int32_t t = bm_lines[i], c = bm_shown[i];
                    if (t == c) continue;
                    int32_t tv = t >= 0x7FFF ? 400 : t <= -0x7FFF ? -400 : t;
                    int32_t cv = c >= 0x7FFF ? 400 : c <= -0x7FFF ? -400 : c;
                    int32_t d = tv - cv;
                    if (d > BM_STEP) cv += BM_STEP;
                    else if (d < -BM_STEP) cv -= BM_STEP;
                    else cv = tv;
                    bm_shown[i] = (int16_t)(cv == tv ? t : cv);
                }
#endif
                for (int i = 0; i < 2 * SCREEN_H; i++) BM_LN[i] = bm_shown[i];
            }
            sh->cvN[0] = N.x0; sh->cvN[1] = N.x1; sh->cvN[2] = N.y0; sh->cvN[3] = N.y1;
            sh->cvI[0] = I.x0; sh->cvI[1] = I.x1; sh->cvI[2] = I.y0; sh->cvI[3] = I.y1;
            sh->cvH[0] = cb->hud.x0; sh->cvH[1] = cb->hud.x1; sh->cvH[2] = cb->hud.y0; sh->cvH[3] = cb->hud.y1;
            sh->cvfull = full;
            sh->cvbi = bi;
            if (sh->bm_game) {            /* (I12) the beam's changed pixels per line (BMS), before the jobs */
                sh->camx = camx; sh->camy = camy;
                bm_spans_make(sh);
#ifdef PICPROF
                pp_n = BMS[0];
#endif
            }
            if (give) {
                int32_t rs = N.y0 + (sh->ovcap ? sh->split_row : CV_TH / 2);
                mrows[0] = N.y0; mrows[1] = rs;
                sh->cvrows[0] = rs; sh->cvrows[1] = N.y1;
            } else if (!fused) {          /* (I17: not for the fused job - it keeps its own master rows:
                                             this overwrote them, so a FUSED master redid every row) */
                mrows[0] = N.y0; mrows[1] = N.y1;
                sh->cvrows[0] = sh->cvrows[1] = 0;
            } else
                sh->cvrows[0] = sh->cvrows[1] = 0;
            cb->valid = 1;
            cb->w = N;
            cb->hud = (trect_t){ 0, 0, 0, 0 };
            if (fused && sh->hud_on) {
#if defined(GAMEPROF) || defined(PROFT) || defined(HUD2)
                cb->hud = (trect_t){ (gx + 3) >> 3, ((gx + 162) >> 3) + 1, (gy + 3) >> 3, ((gy + 30) >> 3) + 1 };
#else
                cb->hud = game_on ? (trect_t){ (gx + 3) >> 3, ((gx + 128) >> 3) + 1, (gy + HUD_Y - 1) >> 3, ((gy + HUD_Y + 13) >> 3) + 1 }
                                  : (trect_t){ (gx + 3) >> 3, ((gx + 162) >> 3) + 1, (gy + 3) >> 3, ((gy + 17) >> 3) + 1 };
#endif
                fused_hud = cb->hud;
            }
        }
        wx_frame(camx);
        sh->phelp = 0;
        if (pipe) {
            int32_t gy = CV_GY(sh, camy);
            *UNCACHED(&pb_next) = 0;
            *UNCACHED(&pb_low) = pb_count(gy);
            *UNCACHED(&pb_go) = -1;
            pipe_gx = CV_GX(sh, camx); pipe_gy = gy; pipe_bi = cur_fb ^ 1;
            sh->phelp = 1;
        }
        if (fused) {
            sh->job = 5;
            spr_ticks0 = frt_read();
        }
        PERF_MARK(6);
        if (spr_defer && !pipe) {         /* (I17) not the 60 FPS mode after all: before the render */
            spr_defer = 0;
            spr_prepare();
        }
        PERF_MARK(14);
        token++;
        COMM(C_TOKEN) = token;

        static shared_t local;            /* (static: the master's stack - 1 KB at 0603F400-F800 - ran
                                             into the RAM below it; m_main only runs on the master) */
        local = *sh;
        PERF_MARK(15);
        if (pipe) {
            /* 60 FPS mode: the slave draws the picture; the master goes on with the next logic - after the
               BG of its bottom rows (I17) */
            if (mrows[0] < mrows[1]) {
                fb_sync();
                cv_band(&local, mrows[0], mrows[1], &cv_mmask[0][0]);
                spr_clear_rows(local.sbi, mrows[0], mrows[1]);
            }
            if (spr_defer) { spr_defer = 0; spr_prepare(); }
            *UNCACHED(&pipe_mbg) = (int32_t)token;     /* (the slave's sprite bands may go on) */
            pipe_vk = vcount;
        } else if (fused) {
            uint16_t f0 = frt_read();
            cv_band(&local, mrows[0], mrows[1], &cv_mmask[0][0]);
            spr_clear_rows(local.sbi, mrows[0], mrows[1]);
            spr_draw_band(local.ssplit, SCREEN_H, local.sbi, local.sgx, local.sgy);
            int32_t mt = (uint16_t)(frt_read() - f0);
            LDST2(10); while (COMM(C_DONE) != token)
                ;
            int32_t wt = (uint16_t)(frt_read() - f0) - mt;
            /* balance: the master has the bottom band - waiting means it needs more rows */
            if (wt > 300 && f_spl > 40) f_spl -= 8;
            else if (mt > *UNCACHED(&sh->spr_sticks) + 1500 && f_spl < SCREEN_H - 40) f_spl += 8;
            sh->job = 0;
            spr_ticks = (uint16_t)(frt_read() - spr_ticks0);
        } else if (local.cv) {
            cv_band(&local, mrows[0], mrows[1], &cv_mmask[0][0]);
        } else {
            render_band(&local, 0, local.split_row);
        }
        dbg_stage = 4;
        uint16_t m_render = (uint16_t)(frt_read() - tm0);
        PERF_MARK(8);
#if defined(GAMEPROF) && defined(PHASE_BG)
        *(volatile uint16_t *)&ares_phase = 0;
#endif
        int lt_slave = local.cv == 1 && game_on && !ripple && !dither_ripple && !rain_n;
        sh->lt_do = 0;
        if (fused) {
            /* (the slave wrote the line table in job 5) */
        } else if (local.cv) {
            if (local.cv == 3) write_line_table_cv3(pm_open);
            else if (lt_slave) {
                /* the slave writes it with its sprite band (job 2) */
                sh->ltx = CV_GX(sh, camx); sh->lty = CV_GY(sh, camy);
                cv_x0par = (sh->ltx + CV_XOFF) & 1;
            }
            else write_line_table_cv(CV_GX(sh, camx), CV_GY(sh, camy), ripple ? sft_tab[cur_fb ^ 1] : 0);
            if (rain_n) {
                /* rain phase: both CPUs restore and draw their half (the slave must be done with its
                   canvas rows first) */
                LDST2(11); while (COMM(C_DONE) != token)
                    ;
                rain_f += game_on ? fs_done : 1;   /* (frame skip: the rain keeps its speed) */
                sh->rbuf = cur_fb ^ 1;
                sh->rH = (uint32_t)(camx + 8 * rain_f) & 255;
                sh->rV = (uint32_t)(-8 * (int32_t)rain_f) & 255;
                sh->rcut = rain_state ? rain_cut : 0;
                sh->rdraw = rain_state < 2;
                sh->rn = rain_n;
                sh->rpix = rain_pix;
                sh->rx0par = cv_x0par;
                sh->job = 1;
                token++;
                COMM(C_TOKEN) = token;
                {
                    shared_t lr = *sh;
                    rain_half(0, &lr, sh);
                }
                LDST2(12); while (COMM(C_DONE) != token)
                    ;
                sh->job = 0;
                if (rain_state == 1 && (rain_cut += 8) >= SCREEN_H) {
                    rain_state = 2;
                    cvb[0].valid = cvb[1].valid = 0;
                    cvb[0].hud = cvb[1].hud = (trect_t){ 0, 0, 0, 0 };   /* (repainted: no old HUD text) */
                }
            }
        } else if (comp) {
            write_line_table_comp(sft_tab[cur_fb ^ 1], cur_fb ^ 1);
        } else if (ripple) {
            write_line_table_ripple(camx & 7, camy & 7, sft_tab[cur_fb ^ 1]);
        } else {
            write_line_table(camx & 7, camy & 7);
        }
        int32_t m_lines = current_line();
        uint16_t lt_ticks = (uint16_t)(frt_read() - tm0) - m_render;

        if (give) {                       /* (I17: the braces - with LDST2 in front, the wait ran in
                                             every mode: the 60 FPS mode's master waited for the
                                             slave's whole picture, no pipeline at all) */
            LDST2(13); while (COMM(C_DONE) != token)
                ;
        }
        dbg_stage = 5;
        if (game_on && ((local.cv == 1 && !dither_ripple) || local.cv == 2) && !fused) {   /* (ripple levels: the sprites wobble with BG1 by 1 px; 2 = perf comp) */
            /* game sprites: both CPUs, split on a canvas tile row */
            uint16_t s0 = frt_read();
            PT(4);
            int32_t gx = CV_GX(sh, camx), gy = CV_GY(sh, camy);
            int bi = cur_fb ^ 1;
            LDST2(14); while (COMM(C_DONE) != token)
                ;
#ifdef CVCHECK2
            {   /* (test) before the sprites: every other pixel of every line against the level data -
                   leftovers of earlier sprites the restore missed show up as errors */
                int32_t wx0 = camx - sh->ox0, wy0 = camy - sh->oy0;
                uint32_t n = 0;
                for (int y = 0; y < SCREEN_H; y++) {
                    uint32_t a = FB_BASE + (uint32_t)((gy + y) & 255) * CV_P + gx + CV_XOFF;
                    for (int x = y & 1; x < 320; x += 2) {
                        if (x < 100 && y < 22) continue;
                        int32_t wx = wx0 + x, wy = wy0 + y;
                        int32_t tx = wx >> 3, ty = wy >> 3, cx = tx >> 2, cy = ty >> 2;
                        uint8_t ex = 0;
                        if (wx >= 0 && wy >= 0 && cx < sh->gw && cy < sh->gh) {
                            uint32_t e = VCH(sh)[sh->grid[cy * sh->gw + cx] * 16 + (ty & 3) * 4 + (tx & 3)];
                            int r = wy & 7;
                            if (e & 0x8000) r = 7 - r;
                            ex = ((const uint8_t *)tile_ptr(sh, e & 0x7FFF))[r * 8 + (wx & 7)];
                        }
                        if (FB8[a + x] != ex) n++;
                    }
                }
                cv2_last = n;
                if (n) cv2_frames++;
            }
#endif
            spr_clear_marks(bi);
            /* split line: balanced between the CPUs (by last frame's band times), on a canvas tile row */
            static int32_t spl = SCREEN_H / 2;
            int32_t split = ((gy + spl + 7) & ~7) - gy;
            sh->sgx = gx; sh->sgy = gy; sh->sbi = bi; sh->ssplit = split;
            sh->lt_do = lt_slave;
            sh->job = 2;
            token++;
            COMM(C_TOKEN) = token;
            uint16_t sm0 = frt_read();
#if defined(GAMEPROF) && defined(PHASE_SPR)
            *(volatile uint16_t *)&ares_phase = 1;      /* (ares profiler: sprite phase) */
#endif
            spr_draw_band(0, split, bi, gx, gy);
            int32_t mt = (uint16_t)(frt_read() - sm0);
            LDST2(15); while (COMM(C_DONE) != token)
                ;
            int32_t wt = (uint16_t)(frt_read() - sm0) - mt;    /* the master's wait for the slave */
            PERF_ADD(11, mt); PERF_ADD(12, wt); PERF_ADD(13, sh->spr_sticks);
#if defined(GAMEPROF) && defined(PHASE_SPR)
            *(volatile uint16_t *)&ares_phase = 0;
#endif
            {   /* (the slave starts late and is interrupted by the music: balance on the wait) */
                int32_t st = sh->spr_sticks;
                if (wt > 300 && spl < SCREEN_H - 40) spl += 8;
                else if (mt > st + 1500 && spl > 40) spl -= 8;
            }
            sh->job = 0;
            spr_ticks = (uint16_t)(frt_read() - s0);
        }
        if (game_on && local.cv == 3 && !fused) {
            /* (I3) Misty Mine: the fog canvas moves per 4-line band (its waves): sprites through this
               buffer's line table; the fog under this buffer's last sprites is redrawn first */
            int16_t (*cv3_dirty)[CV3_MAXR][4] = CV3_DIRTY;
            int16_t (*sr)[4] = CV3_SR;
            int32_t *cv3_nd = CV3_ND;
            int bi = cur_fb ^ 1;
            LDST2(14); while (COMM(C_DONE) != token)
                ;
            if (cvb[bi].valid && cv3_nd[bi] > 0 && cv3_nd[bi] <= CV3_MAXR)
                for (int k = 0; k < cv3_nd[bi]; k++) {
                    int32_t rr[4] = { cv3_dirty[bi][k][0], cv3_dirty[bi][k][1], cv3_dirty[bi][k][2], cv3_dirty[bi][k][3] };
                    /* (only rectangles a sprite can cover: 64 x 64 px plus the waves' offsets) */
                    if (rr[1] <= rr[0] || rr[3] <= rr[2] || rr[1] - rr[0] > 16 || rr[3] - rr[2] > 16 ||
                        rr[0] < 0 || rr[1] > 4000) continue;    /* (the canvas x range: cv_xbase + 32000) */
                    render_rect(&local, rr);
                }
            int n = spr_draw_lt(FB16, sr, CV3_MAXR);
            if (n < 0) { cvb[bi].valid = 0; n = 0; }       /* (too many: the whole fog next time) */
            for (int k = 0; k < n; k++) {
                int32_t x0 = 0x7FFFFFFF, x1 = -0x7FFFFFFF, l0 = 0x7FFFFFFF, l1 = -0x7FFFFFFF;
                for (int i = sr[k][2] >> 2; i <= (sr[k][3] - 1) >> 2 && i < SCREEN_H / 4; i++) {
                    int32_t ox = comp_wave[i][0] - cv_xbase, oy = comp_wave[i][1];   /* (I16: as the line table, no parity row since I14) */
                    int32_t ya = sr[k][2] > 4 * i ? sr[k][2] : 4 * i, yb = sr[k][3] < 4 * i + 4 ? sr[k][3] : 4 * i + 4;
                    if (ox + sr[k][0] < x0) x0 = ox + sr[k][0];
                    if (ox + sr[k][1] > x1) x1 = ox + sr[k][1];
                    if (oy + ya < l0) l0 = oy + ya;
                    if (oy + yb > l1) l1 = oy + yb;
                }
                cv3_dirty[bi][k][0] = x0 >> 3; cv3_dirty[bi][k][1] = (x1 + 7) >> 3;
                cv3_dirty[bi][k][2] = l0 >> 3; cv3_dirty[bi][k][3] = (l1 + 7) >> 3;
            }
            cv3_nd[bi] = n;
        }
        dbg_stage = 6;
        if (local.cv && local.ovcap) {    /* this frame's overlay marks for this buffer */
            const uint32_t *sm = (const uint32_t *)((uint32_t)&cv_smask[0][0] | 0x20000000);
            const uint32_t *mm = &cv_mmask[0][0];
            uint32_t *om = (uint32_t *)((uint32_t)&cv_ovd[cur_fb ^ 1][0][0] | 0x20000000);
            for (int i = 0; i < 64; i++) om[i] = sm[i] | mm[i];
        }
#ifdef CVCHECK
        if (local.cv) {   /* self test: sampled screen pixels against the level data */
            int32_t gx = camx - sh->ox0, gy = camy - sh->oy0;
            const uint8_t *st = ripple ? sft_tab[cur_fb ^ 1] : 0;
            for (int y = 30; y < SCREEN_H; y += 3) {
                int d = ripple ? ripple_pat[(y - ripple_phase) & 63] : 0;
                uint32_t a = (uint32_t)FB16[y] * 2 + (st ? st[y] : (camx & 1));
                for (int x = (y * 7) % 5; x < 320; x += 5) {
                    int32_t wx = gx + d + x, wy = gy + d + y;
                    int32_t tx = wx >> 3, ty = wy >> 3, cx = tx >> 2, cy = ty >> 2;
                    uint8_t ex = 0;
                    if (wx >= 0 && wy >= 0 && cx < sh->gw && cy < sh->gh) {
                        uint32_t e = VCH(sh)[sh->grid[cy * sh->gw + cx] * 16 + (ty & 3) * 4 + (tx & 3)];
                        int r = wy & 7;
                        if (e & 0x8000) r = 7 - r;
                        ex = ((const uint8_t *)tile_ptr(sh, e & 0x7FFF))[r * 8 + (wx & 7)];
                    }
                    if (x < 170 && y < 32) continue;
                    if (FB8[a + x] != ex) cv_errors++;
                }
            }
        }
#endif
        if (comp && frt_ok) {
            /* balance the bands: the master also writes the line table and HUD (~1.5k ticks) */
            int32_t diff = (int32_t)m_render + lt_ticks + 300 - (int32_t)sh->slave_ticks;
            int32_t sp = sh->split;
            if (diff > 800 && sp > 48) sp -= 2;
            else if (diff < -800 && sp < 176) sp += 2;
            sh->split = sp;
        } else if (frt_ok && (!local.cv || local.ovcap)) {
            /* same for tile rows (the slave also mixes the music) */
            int32_t diff = (int32_t)m_render + lt_ticks + 300 - (int32_t)sh->slave_ticks;
            int32_t sr = sh->split_row;
            if (diff > 1500 && sr > 6) sr--;
            else if (diff < -1500 && sr < TROWS - 6) sr++;
            sh->split_row = sr;
        }
        uint16_t th0 = frt_read();
        PT(5);
#ifdef OLDHUD
        if (0) {
#else
        if (comp) {
#endif
            /* the 68000 draws the HUD on Genesis plane A (free for the SH-2s) */
            /* COMM10: bit 15 HUD on, bits 13-12 field (0 FPS, 1 master %, 2 slave %), bits 7-0 value */
            int fsel = frame_no % 3;
#ifdef MUSDEBUG
            {   /* M = voice part, S = whole refill (FRT ticks / 100, per refill) */
                static uint32_t lt, ln, avg, l0, a0;
                extern volatile uint32_t dsp_prof[4];
                uint32_t tt = *UNCACHED(&music_ticks), nn = *UNCACHED(&music_refills);
#ifdef MUSPROF2
                uint32_t p0 = UNCACHED(&dsp_prof[0])[2];
                tt = UNCACHED(&dsp_prof[0])[1];
#else
                uint32_t p0 = UNCACHED(&dsp_prof[0])[0];
#endif
                if (nn - ln >= 100) { avg = (tt - lt) / (nn - ln); a0 = (p0 - l0) / (nn - ln); lt = tt; l0 = p0; ln = nn; }
                s_pct = avg / 100 > 99 ? 99 : avg / 100;
                m_pct = a0 / 100 > 99 ? 99 : a0 / 100;
            }
#endif
            COMM(10) = (show_hud ? 0x8000 : 0) | (fsel << 12) | (fsel == 0 ? fps : fsel == 1 ? m_pct : s_pct);
        } else if (show_hud) {
            if (game_on) {   /* game levels: short form "F60 M98 S27" (each glyph costs ~0.25% of a frame) */
                line[0] = 'F'; fmt_num(line + 1, fps, 2); line[3] = ' ';
                line[4] = 'M'; fmt_num(line + 5, m_pct, 2); line[7] = ' ';
                line[8] = 'S'; fmt_num(line + 9, s_pct, 2); line[11] = ' ';
                line[12] = 'G'; fmt_num(line + 13, m68_pct > 99 ? 99 : (int)m68_pct, 2); line[15] = 0;
            } else {
                line[0] = 'F'; line[1] = 'P'; line[2] = 'S'; line[3] = ' ';
                fmt_num(line + 4, fps, 2);
                line[6] = ' '; line[7] = 'M'; line[8] = ' ';
                fmt_num(line + 9, m_pct, 2);
                line[11] = '%'; line[12] = ' '; line[13] = 'S'; line[14] = ' ';
                fmt_num(line + 15, s_pct, 2);
                line[17] = '%'; line[18] = 0;
            }
            if (local.cv == 3) {
                /* (Misty Mine's wave bands: text written through this frame's line table lands on canvas
                   lines that later frames show elsewhere - white blocks over the fog.  No HUD there.) */
            } else if (local.cv) {
                int32_t gx = CV_GX(sh, camx), gy = CV_GY(sh, camy);
#if defined(GAMEPROF) || defined(PROFT) || defined(HUD2)
                cvb[cur_fb ^ 1].hud = (trect_t){ (gx + 3) >> 3, ((gx + 170) >> 3) + 1, (gy + 3) >> 3, ((gy + 30) >> 3) + 1 };
#else           /* game builds: one short HUD line (FPS, master %, slave %) */
                cvb[cur_fb ^ 1].hud = game_on ? (trect_t){ (gx + 3) >> 3, ((gx + 128) >> 3) + 1, (gy + HUD_Y - 1) >> 3, ((gy + HUD_Y + 13) >> 3) + 1 }
                                              : (trect_t){ (gx + 3) >> 3, ((gx + 162) >> 3) + 1, (gy + 3) >> 3, ((gy + 30) >> 3) + 1 };
#endif
                if (local.cv == 3) cv3_rect(&cvb[cur_fb ^ 1].hud, 1, 8, 3, 162);
                if (fused) cvb[cur_fb ^ 1].hud = fused_hud;
                else hud_text_cv(4, game_on ? HUD_Y : 4, line);
                /* (fused: the slave drew it in job 5; the rect was set with the job) */
            } else if (comp) hud_text_fast(4, 4, line); else hud_text(4, game_on ? HUD_Y : 4, camx & 7, camy & 7, line);
#ifdef PROF
            for (int k = 0; k < 5; k++) { fmt_num(line + k * 4, prof[k] / 100, 3); line[k * 4 + 3] = ' '; }
            line[19] = 0;
#else
#ifdef MUSDEBUG
            {
                static uint32_t lt, ln, avg;
                uint32_t tt = *UNCACHED(&music_ticks), nn = *UNCACHED(&music_refills);
                if (nn - ln >= 100) { avg = (tt - lt) / (nn - ln); lt = tt; ln = nn; }
                line[0] = 'R';
                fmt_num(line + 1, sh->split_row, 2);
                line[3] = ' ';
                fmt_num(line + 4, (int)avg, 5);
                line[9] = ' ';
                fmt_num(line + 10, stack_free(0x0603F800), 4);
                line[14] = ' ';
                fmt_num(line + 15, stack_free(0x0603E810), 4);
                line[19] = 0;
            }
            if (0) {
#else
            {
#endif
            line[0] = 'X'; line[1] = ' ';
#ifdef SHOWSLOTS
            fmt_num(line + 2, nslots, 5);
#else
            fmt_num(line + 2, camx, 5);
#endif
            line[7] = ' '; line[8] = 'Y'; line[9] = ' ';
            fmt_num(line + 10, camy - 1, 5);
            line[15] = 0;
            if (game_on) {
                if (game_stats.err) {
                    static const char hx[] = "0123456789ABCDEF";
                    line[0] = 'E'; line[1] = 'R'; line[2] = 'R'; line[3] = ' ';
                    for (int k = 0; k < 8; k++) line[4 + k] = hx[(game_stats.err >> (28 - 4 * k)) & 15];
                    line[12] = 0;
                } else {
                    line[0] = 'G'; line[1] = ' ';
                    fmt_num(line + 2, game_ticks * 100 / FRAME_TICKS > 99 ? 99 : game_ticks * 100 / FRAME_TICKS, 2);
                    line[4] = '%'; line[5] = ' ';
                    fmt_num(line + 6, spr_ticks * 100 / FRAME_TICKS, 2);
                    line[8] = '%'; line[9] = ' ';
                    fmt_num(line + 10, (int)spr_stats.drawn, 3);
                    line[13] = ' ';
                    fmt_num(line + 14, (int)game_avg, 3);
#ifdef STACKCHK
                    {   /* deepest master stack use since the level start (bytes below 0x0603F800) */
                        extern char _end[];
                        static uint32_t used;
                        if ((game_stats.frames & 63) == 0) {
                            volatile uint32_t *p = (volatile uint32_t *)(((uint32_t)_end + 16) | 0x20000000);
                            while (*p == 0xA5A5A5A5) p++;
                            used = 0x2603F800 - (uint32_t)p;
                        }
                        fmt_num(line + 10, (int)used, 5);
                        line[15] = 0;
                    }
#endif
                    line[17] = 0;
                }
            }
            }
#endif
#ifdef CVPROF
            {
                static uint16_t pm, pl, ph, pn;
                pm = m_render; pl = lt_ticks; ph = hud_ticks; pn = 0;
                fmt_num(line, pm, 5); line[5] = ' '; fmt_num(line + 6, pl, 4); line[10] = ' ';
                fmt_num(line + 11, ph, 4); line[15] = 0;
                (void)pn;
            }
#endif
#ifdef GAMEPROF
            if (game_on) {   /* averages over 64 frames, % of a frame: G logic, B BG strips, S sprites, T master total */
                /* fixed window: game frames 60..359 of the GAMEDEMO script (comparable between builds) */
                static uint32_t ag, ab, al, as, at, an, dg, db_, dl, ds, dt;
                if (game_stats.frames >= 60 && game_stats.frames < 360) {
#ifdef J3HUD
                    ag += game_ticks; ab += m_render; al += sh->j3_ticks; as += spr_ticks; at += frt_since(t0, vc_t0); an++;
#else
                    ag += game_ticks; ab += m_render; al += lt_ticks; as += spr_ticks; at += frt_since(t0, vc_t0); an++;
#endif
                }
#ifdef PICPROF
                /* (test) this picture's own phases (no average): logic steps in the first digit pair */
                dg = game_ticks * 100 / FRAME_TICKS; db_ = m_render * 100 / FRAME_TICKS; dl = lt_ticks * 100 / FRAME_TICKS;
                ds = spr_ticks * 100 / FRAME_TICKS; dt = frt_since(t0, vc_t0) * 100 / FRAME_TICKS;
                if (dg > 99) dg = 99;
                if (db_ > 99) db_ = 99;
                if (dl > 99) dl = 99;
                if (ds > 99) ds = 99;
                if (dt > 999) dt = 999;
                an = 0;
#endif
                if (an && (game_stats.frames == 360 || (an & 63) == 0)) {
                    dg = ag * 100 / an / FRAME_TICKS; db_ = ab * 100 / an / FRAME_TICKS; dl = al * 100 / an / FRAME_TICKS;
                    ds = as * 100 / an / FRAME_TICKS; dt = at * 100 / an / FRAME_TICKS;
                }
                fmt_num(line, dg > 99 ? 99 : dg, 2); line[2] = ' ';
                fmt_num(line + 3, db_, 2); line[5] = ' ';
                fmt_num(line + 6, dl, 2); line[8] = ' ';
                fmt_num(line + 9, ds, 2); line[11] = ' ';
                fmt_num(line + 12, dt, 3); line[15] = 0;
            }
#endif
#ifdef PROFT
            for (int i = 0; i < 7; i++) { fmt_num(line + i * 3, proft_pct[i] > 99 ? 99 : proft_pct[i], 2); line[i * 3 + 2] = ' '; }
            line[20] = 0;
#endif
#ifdef CVCHECK
            line[0] = 'X'; line[1] = ' '; fmt_num(line + 2, cv_errors > 99999 ? 99999 : cv_errors, 5);
            line[7] = ' '; fmt_num(line + 8, cur_idx, 2); line[10] = 0;
#endif
#ifdef CVCHECK2
            line[0] = 'X'; line[1] = ' '; fmt_num(line + 2, cv2_last > 99999 ? 99999 : cv2_last, 5);
            line[7] = ' '; fmt_num(line + 8, cv2_frames > 9999 ? 9999 : cv2_frames, 4); line[12] = 0;
#endif
            if (music_msg) {
                static const char *const names[3] = { "MUSIC AUTO", "MUSIC LIGHT", "MUSIC OFF" };
                const char *q = names[music_mode];
                int i = 0;
                for (; q[i]; i++) line[i] = q[i];
                for (; i < 15; i++) line[i] = ' ';
                line[15] = 0;
                music_msg--;
            }
#if defined(GAMEPROF) || defined(PROFT) || defined(HUD2)
            if (local.cv) hud_text_cv(4, 15, line);
#else
            if (local.cv) { if (!game_on) hud_text_cv(4, 15, line); }
#endif
            else if (comp) hud_text_fast(4, 16, line); else hud_text(4, 15, camx & 7, camy & 7, line);
        }
        if (pm_open && local.cv >= 1 && local.cv <= 3 && in_level) {   /* (I11: also the fog levels, 2 / 3) */
            if (pm_key != PM_KEY()) { pm_key = PM_KEY(); pm_shown = 0; }
            pm_shown++;
            static const char *const it[PM_N] = { "RESUME", "FRAME SKIP", "60FPS MODE", "FOREGROUND", "RIPPLE",
                                                  "OBJECT RANGE", "STATS", "EXIT TO MAP", "LEVEL SELECT" };
            int32_t gx = CV_GX(sh, camx), gy = CV_GY(sh, camy);
            /* (I21) small font: 6 px characters, 10 px lines */
            const int bx = 92, by = 64, bw = 136, bh = 22 + PM_N * 10;
            box_cv(bx, by, bw, bh, 96);
            text57_cv(bx + 47, by + 5, "OPTIONS", 112, 96);
            for (int i = 0; i < PM_N; i++) {
                int yy = by + 18 + i * 10;
                const char *v = 0;
                if (i == pm_sel) text57_cv(bx + 6, yy, ">", 112, 96);
                text57_cv(bx + 16, yy, it[i], 112, 96);
                if (i == PM_FSKIP) v = fskip ? "ON" : "OFF";
                if (i == PM_PIPE) v = pipe_mode ? "ON" : "OFF";
                if (i == PM_FG) v = fg_on ? "ON" : "OFF";
                if (i == PM_RIPPLE) v = rip_on ? "ON" : "OFF";
                if (i == PM_OBJ) v = obj_wide ? "WIDE" : "SNES";
                if (i == PM_STATS) v = pm_stats ? "ON" : "OFF";
                if (v) text57_cv(bx + 100, yy, v, 112, 96);
            }
            /* restore the whole area next time this buffer is drawn (with the HUD's); the fog canvases
               have no HUD restore: the whole buffer then (the game is paused) */
            if (local.cv == 1)
                cvb[cur_fb ^ 1].hud = (trect_t){ (gx + 3) >> 3, ((gx + bx + bw) >> 3) + 1, (gy + 3) >> 3, ((gy + SCREEN_H) >> 3) + 1 };   /* (I21c: down to the STATS line)*/
            else
                cvb[cur_fb ^ 1].valid = 0;
        }
        uint16_t t1 = frt_read();
        hud_ticks = (uint16_t)(t1 - th0);
        if (game_on && frt_ok && in_level) {
            /* frame skip estimates: logic per game frame, the rest per picture (EMA 1/8) */
            int32_t busy = (int32_t)frt_since(t0, vc_t0), gl = (int32_t)game_ticks / (fs_done ? fs_done : 1);
            int32_t rr = busy - (int32_t)game_ticks - (int32_t)pipe_ticks - (int32_t)fb_wait_ticks;
            if (pipe_ticks) {             /* 60 FPS mode: a picture takes the logic + the master's sprite
                                             bands / the wait for the slave + the work after the flip
                                             (sprite list, BG setup, this bookkeeping) - with a margin:
                                             a picture that misses its vblank costs a whole frame */
                gl = (int32_t)pipe_logic_last / (fs_done ? fs_done : 1);
                rr = (int32_t)pipe_rest + FRAME_TICKS / 16;
            }
            if (rr < 0) rr = 0;
            if (!fs_r) { fs_l = gl; fs_r = rr; }
            fs_l += (gl - fs_l) >> 3; fs_r += (rr - fs_r) >> 3;
        }
        if (t1 != t0) {
            {   /* shown as averages over ~16 frames (x16 fixed point) */
                static uint32_t ma, sa;
                uint32_t mp = (frt_since(t0, vc_t0) - pipe_ticks) * 100 / FRAME_TICKS, sp = sh->slave_ticks * 100 / FRAME_TICKS;
#ifdef J3S
                sp = (uint32_t)*UNCACHED(&sh->j3_ticks) * 100 / FRAME_TICKS;    /* (measurement ROMs: S = the slave's early job) */
#endif
                if (mp > 99) mp = 99;
                ma += (int32_t)(mp * 16 - ma) >> 4; sa += (int32_t)(sp * 16 - sa) >> 4;
                m_pct = (ma + 8) >> 4; s_pct = (sa + 8) >> 4;
            }
        } else {
            m_pct = (38 + m_lines) * 100 / 262;
            s_pct = (38 + sh->slave_lines) * 100 / 262;
        }
        if (m_pct > 99) m_pct = 99;
        if (s_pct > 99) s_pct = 99;
#ifdef FSHUD
        s_pct = fs_rate > 99 ? 99 : fs_rate;
#endif
#ifdef FSDBG
        m_pct = fs_l * 100 / FRAME_TICKS; s_pct = fs_r * 100 / FRAME_TICKS;
        if (m_pct > 99) m_pct = 99;
        if (s_pct > 99) s_pct = 99;
#endif
#ifdef RAINCHECK
        m_pct = rc_y; s_pct = rc_bad > 99 ? 99 : rc_bad;
#endif

        if (calib_frames >= 32 || !frt_ok) {
#if defined(PROFH) || defined(MASTER_SKY)
            REG8(SYS_INTMASK) = hint_mask;
#else
#ifndef PIPE_MHINT
#define PIPE_MHINT 0
#endif
            if (pipe && PIPE_MHINT) {     /* 60 FPS mode: the slave draws everything, the master the sky */
                *UNCACHED(&slave_hmask) = 0;
                REG8(SYS_INTMASK) = hint_mask;
            } else {
                REG8(SYS_INTMASK) = 0x08; /* V only: the slave does the per-line work */
                *UNCACHED(&slave_hmask) = (hint_mask & 0x04) ? 0x0C : 0;
            }
#endif
        }
        PERF_MARK(9);
        dbg_stage = 7;
        PT(6);
        if (pipe) { pipe_pend = 1; pipe_cx = camx; pipe_cy = camy - 1; }
        else {
            pillar_draw();
            /* (I17) gameplay pictures: the swap can happen while the next logic frame runs */
            present2(camx, camy - 1, &frame_no, game_on && in_level && map_mode == 0 && !pm_open && !fe_vm &&
                     !game_fblank && game_in_main_loop());
        }
#ifdef PROFT
        {   /* (game frames 60..359: % of a frame between the stamps) */
            static uint32_t acc[7], an;
            uint16_t p7 = frt_read();
            if (game_on) {                     /* (every 60 shown frames: their means) */
                for (int i = 0; i < 6; i++) acc[i] += (uint16_t)(pt[i + 1] - pt[i]);
                acc[6] += (uint16_t)(p7 - pt[6]);
                an++;
            }
#ifdef PICPROF
            /* (test) this picture's phases in tenths of a frame (vcount-corrected: long stalls show) */
            for (int i = 0; i < 7; i++) {
                uint16_t a = pt[i], b = i < 6 ? pt[i + 1] : p7;
                uint32_t va = ptv[i], vb = i < 6 ? ptv[i + 1] : vcount;
                uint32_t d = vb == va ? (uint16_t)(b - a) : (vb - va) * FRAME_TICKS + (uint16_t)(b - a);
                proft_pct[i] = d * 10 / FRAME_TICKS;
            }
            proft_pct[0] = pp_full; proft_pct[5] = pp_n / 4;
            an = 0;
#endif
            if (an == 60) { for (int i = 0; i < 7; i++) { proft_pct[i] = acc[i] * 100 / an / FRAME_TICKS; acc[i] = 0; } an = 0; }
        }
#endif
        PERF_MARK(10);
        dbg_stage = 8;
        swaps++;
        if (vcount - fps_mark >= (pal ? 50u : 60u)) {
            fps = swaps;
#ifdef FSHUD
            {   /* (test: S field = game frames in the last 60 vblanks) */
                static uint32_t lf;
                fs_rate = game_stats.frames - lf; lf = game_stats.frames;
            }
#endif
            swaps = 0;
            fps_mark += pal ? 50 : 60;
            /* adaptive music: if this level can't hold 60 (PAL 50) fps, mix the voices at half rate */
            if (++level_seconds > 2 && fps < (pal ? 48u : 57u) && music_mode == 0)
                *UNCACHED(&music_light) = 1;
        }
    }
}

#undef token
/* ------------------------------------------------------------ slave */
void s_main(void)
{
    {
        extern uint8_t wram_lo[];
        __builtin_set_thread_pointer(wram_lo);
    }
#ifdef STACKCHK
    {   /* (test) paint the slave stack area (0603F800 - its stack pointer) */
        uint32_t sp;
        __asm__ volatile ("mov r15,%0" : "=r"(sp));
        for (volatile uint32_t *p = (volatile uint32_t *)0x2603F800; (uint32_t)p < ((sp - 128) | 0x20000000); p++)
            *p = 0x5A5A5A5A;
    }
#endif
    shared_t *sh = UNCACHED(&shared_state);
    uint16_t last = COMM(C_TOKEN);
    int32_t epoch = -1;
    FRT_TCR = 0;
#ifdef MUSDEBUG
    stack_fill(0x0603F800);
#endif
    {
        const pack_hdr_t *ph = PK(pack_hdr_t, 0);
        if (ph->off_music)
            music_init(PACK + ph->off_music, slot_echo.echo);
    }
#ifdef BENCH
    for (;;) {
        uint32_t m = *UNCACHED(&bench_slave);
        if (m == 1) b_rd(0x0602C000, 16, 2048);
        else if (m == 2) b_rd(0x02300000, 16, 2048);
    }
#endif
    uint32_t hmask = 0;
    for (;;) {
        *UNCACHED(&s_job_now) = 0;
#ifdef PERFREC
        {   uint16_t w0 = frt_read();
            while (COMM(C_TOKEN) == last)
                ;
            *UNCACHED(&perf_sidle) += (uint16_t)(frt_read() - w0);   /* (waits > 1 FRT wrap undercount) */
        }
#endif
        while (COMM(C_TOKEN) == last)
            ;
        last = COMM(C_TOKEN);
        {
            uint32_t m = *UNCACHED(&slave_hmask);
            if (m != hmask) { hmask = m; REG8(SYS_INTMASK) = (REG8(SYS_INTMASK) & 0x01) | m; }
        }
        static shared_t local;            /* (static: the slave's 2 KB stack; s_main is the slave's only) */
        local = *sh;
        *UNCACHED(&s_job_now) = 0x100 | local.job;
#ifdef TCACHE
        local.tcd = tc_sdata; local.tct = tc_stag; local.tcm = TC_SN - 1; local.tcn = TC_SN;
#endif
        if (local.epoch != epoch) {           /* new level: drop stale cache lines */
            epoch = local.epoch;
            CCR = 0x10;
            CCR = *UNCACHED(&comp) ? 0x19 : 0x11;
        }
        if (local.job == 3) {                 /* early restore under the old sprites (game frames) */
            while ((REG16(VDP_FBCTL) & 1) == local.pbi)     /* (I17: until the swap - FLIP_ASYNC) */
                ;
            uint16_t j0 = frt_read();
            {   /* under the old HUD, inside the valid window */
                const int32_t *W = local.pw, *H = local.ph;
                int32_t r[4] = { H[0] > W[0] ? H[0] : W[0], H[1] < W[1] ? H[1] : W[1],
                                 H[2] > W[2] ? H[2] : W[2], H[3] < W[3] ? H[3] : W[3] };
                if (r[0] < r[1] && r[2] < r[3]) render_rect(&local, r);
            }
            cv_restore_spr(&local);
#ifndef NO_EARLY_OV
            if (local.ovcap) cv_restore_ov(&local);
#endif
            if (local.pdraw) {
                /* the strips entering the predicted window (the overlay pass comes after the logic) */
                local.spr = 0;
                local.ovcap = 0;
                local.nov = 0;
                cv_band(&local, local.cvN[2], local.cvN[3], &cv_smask[0][0]);
            }
            sh->j3_ticks = (uint16_t)(frt_read() - j0);
            COMM(C_DONE) = last;
            continue;
        }
        if (local.job == 5) {                 /* fused band (simple game levels): BG, line table, sprites, HUD */
            while ((REG16(VDP_FBCTL) & 1) == local.sbi)     /* (I17: until the swap - FLIP_ASYNC) */
                ;
            uint16_t r0 = frt_read();
            cv_band(&local, local.frows[0], local.frows[1], &cv_smask[0][0]);
            spr_clear_rows(local.sbi, local.frows[0], local.frows[1]);
            write_line_table_cv(local.ltx, local.lty, 0);
            spr_slave_sync();
            sh->j3_ticks = (uint16_t)(frt_read() - r0);     /* (J3S builds: S = the BG part) */
            if (local.phelp) {
                /* 60 FPS mode: sprite bands from the top, the master takes them from the bottom */
                int32_t nb = pb_count(local.sgy);
                *UNCACHED(&pb_go) = (int32_t)last;
                for (;;) {
                    int32_t b = *UNCACHED(&pb_next);
                    if (b >= nb || b >= *UNCACHED(&pb_low)) break;
                    *UNCACHED(&pb_next) = b + 1;
                    while (*UNCACHED(&pipe_mbg) != (int32_t)last)     /* (the master's BG rows + sprite list) */
                        ;
                    spr_draw_band(pb_edge(local.sgy, b), pb_edge(local.sgy, b + 1), local.sbi, local.sgx, local.sgy);
                }
            } else
                spr_draw_band(0, local.ssplit, local.sbi, local.sgx, local.sgy);
            if (local.hud_on) hud_text_cv(4, game_on ? HUD_Y : 4, local.hud);
            uint16_t dt = (uint16_t)(frt_read() - r0);
            sh->spr_sticks = dt;
            sh->slave_ticks = dt;
            COMM(C_DONE) = last;
            continue;
        }
        if (local.job == 2) {                 /* game sprites: the bottom band */
            uint16_t r0 = frt_read();
            if (local.lt_do) write_line_table_cv(local.ltx, local.lty, 0);
            spr_slave_sync();
            spr_draw_band(local.ssplit, SCREEN_H, local.sbi, local.sgx, local.sgy);
            sh->spr_sticks = (uint16_t)(frt_read() - r0);
            sh->slave_ticks += (uint16_t)(frt_read() - r0);
            COMM(C_DONE) = last;
            continue;
        }
        if (local.job == 1) {                 /* rain phase: the bottom half */
            uint16_t r0 = frt_read();
            rain_half(1, &local, sh);
            sh->slave_ticks += (uint16_t)(frt_read() - r0);
            COMM(C_DONE) = last;
            continue;
        }
        uint16_t t0 = frt_read();
        render_band(&local, local.split_row, TROWS);
        sh->slave_ticks = (uint16_t)(frt_read() - t0);
        sh->slave_lines = *UNCACHED(&hline) - 1;
        COMM(C_DONE) = last;
    }
}
