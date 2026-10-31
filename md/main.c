/*
 * DKC 32X viewer v2 - 68000 side.
 *
 * Owns the Genesis VDP: loads each level's background planes from the pack (ROM 0x040000,
 * read through the 32X bank window at 0x900000) when the master SH-2 asks (COMM6), scrolls
 * them every vblank from the camera the SH-2 announced, and runs the per-line jungle effects.
 * H40 + shadow/highlight: low-priority tiles are shadowed (SNES half-colour math).
 */
#include <stdint.h>

#define VDP_DATA   (*(volatile uint16_t *)0xC00000)
#define VDP_CTRL   (*(volatile uint16_t *)0xC00004)
#define VDP_CTRL32 (*(volatile uint32_t *)0xC00004)
static uint16_t md_pal, md_palph;      /* 50 Hz console (VDP status bit 0), md_setup */
#define VDP_VCNT   (*(volatile uint8_t *)0xC00008)
#define COMM(n)    (*(volatile uint16_t *)(0xA15120 + (n)))
#define BANKREG    (*(volatile uint16_t *)0xA15104)
#define IO_DATA1   (*(volatile uint8_t *)0xA10003)

#define C_CAMX   0
#define C_CAMY   2
#define C_FRAME  4
#define C_CMD    6
#define C_PAD    8
#define C_HUD    10   /* comp levels: bit15 HUD on, bits 14-8 master %, 6-0 slave % */

#define RAM_CODE __attribute__((section(".ramtext")))   /* code section: alignment pads are NOPs */
#define PACK_ROM 0x040000UL

#define VRAM_PLANE_A 0xA000
#define VRAM_PLANE_B 0xC000
#define VRAM_SPRITES 0xE000
#define VRAM_HSCROLL 0xE400

static uint16_t bg3buf[2][224];          /* (I14) double-buffered: md_idle fills the other one, md_vint swaps */
static uint16_t *bg3col = bg3buf[0], *bg3nxt = bg3buf[1];
static volatile uint16_t bg3_swap;
static uint16_t bg3day[224], bg3tgt[224], bg3fade, bg3k, bg3want;   /* jungle sunset: BG3 haze gradient */
static uint16_t cur_bg3col;
static uint16_t wx_ram[6 * 64];         /* weather palette keyframes: Genesis CRAM [nkey][64] */
static const uint16_t *wx_tab;          /* wx_ram or 0 */
static uint16_t wx_shift, wx_nkey, anim_en = 3, anim_blank;
static uint16_t wx_jungle;                 /* (I14) weather block kind 4: jungle darkness (+ Ropey's rain) */
static uint16_t altB, altB_on, scB_main[6], scB_alt[6];  /* weather snow: 2nd plane B map at $8000 */
static uint16_t wx_cur;
static uint16_t wx_buf[64], wx_buf_ready;
static uint16_t last_frame;
/* (I13) after a level's planes are loaded the display stays off until the SH-2's second picture of the
   level (C_FRAME changed twice): its 32X layer goes on at that same flip (main.c disp_pending), and
   the first frame's weather colours (CRAM, BG3 haze) are in by then - the planes were shown alone,
   half built, for a few frames, then once with the day sky (Jungle Hijinxs) */
static uint16_t disp_hold, hold_f;
static uint16_t no_ripple;                 /* (I14) comp levels: BG1 without the underwater ripple */
static uint16_t hcount;                 /* H-ints since vblank: the first one ends line 0 */
static uint16_t active, fx, planes, hud_shown;
static uint16_t scA[6], scB[6];
static const uint16_t *anim_data;       /* animated plane tiles: n x {tile, 8 frames x 16 words} */
static uint16_t anim_n, anim_frame;
/* weather snow, state 5: the blizzard on plane B (map at VRAM $6000) in front of the BG2 trees */
static const uint16_t *hv_frames;
static const uint8_t *hv_sway;
static uint16_t hv_tile, hv_on, hv_f;
/* walkway lamps (md flag 64): the light cones on plane A (BG2 on B), streamed per lamp as the view approaches;
   per-line H scroll = the lamp's swing (skew about row lc_pivot) + the checker phase (1 px, odd frames) */
static const uint16_t *lc_pos, *lc_map, *lc_cols, *lc_swing;
static uint16_t lc_n, lc_pivot, lc_on, lc_have, lc_t, lc_d, lc_vy, lc_ph;
static int16_t lc_op = -1;                  /* lamp being written / cleared, its next row */
/* (I21f) lamp levels' flicker checkerboard (md flag 256): CRAM lines 0-2 alternate between the block's
   CRAM (A) and a second one (B) every vblank - colours between two Genesis levels show as a 1-px
   checkerboard of the two that swaps every frame (build_viewer genesis_planes, flick) */
static const uint16_t *fl_a, *fl_b;
static uint16_t fl_on, fl_ph, fl_k, fl_sel;
static uint16_t lc_row, lc_clear;
/* Slipslide glimmer (md flag 128): highlight sprites over the BG2 crystals, timed like the SNES
   (CODE glow routine: 0x60-0x9F frames apart, 4 glimmers x 8 frames at new random positions) */
static const uint16_t *gl_blk;
static uint16_t gl_n, gl_t, gl_rng = 0xACE1, gl_x, gl_y, gl_shown = 0xFFFF, gl_on;
static int16_t gl_base;
/* Torchlight: Squawks' beam = S/H highlight sprites over the shadowed plane B (SAT at bob 0 in RAM) */
static uint16_t bm_sat[64 * 4], bm_n, bm_lvl, bm_on, bm_shown = 0xFFFF;
/* the apex the SH-2 sends (COMM10 = 0x4000 | x / 2 << 8 | y, y 0xFF: off); the SAT template is
   built at the builder's apex (LIGHTS 0x14 beam = 64, 110) and moved with it */
#define BM_AX0 64
#define BM_AY0 110
static int16_t bm_ax = BM_AX0, bm_ay = BM_AY0;

volatile uint16_t dbg_v, dbg_h, dbg_c;
volatile uint16_t dbg_comm[8];      /* COMM0..14 snapshot each vblank (debug aid) */

static inline void vram_w(uint16_t a)
{
    VDP_CTRL32 = 0x40000000UL | ((uint32_t)(a & 0x3FFF) << 16) | (a >> 14);
}
static inline void cram_w(uint16_t a)
{
    VDP_CTRL32 = 0xC0000000UL | ((uint32_t)(a & 0x3FFF) << 16);
}
static inline void vsram_w(uint16_t a)
{
    VDP_CTRL32 = 0x40000010UL | ((uint32_t)(a & 0x3FFF) << 16);
}

static void bm_write(void)
{
    /* (I10) the apex comes from the SH-2: the game's own window (Squawks' position) or the viewer's
       bob - the 32X layer and these sprites move together */
    int16_t dx = bm_ax - BM_AX0, dy = bm_ay - BM_AY0;
    uint16_t key = bm_on ? (uint16_t)((uint16_t)bm_ax << 8 | (uint8_t)bm_ay) : 0xFFFE;
    if (key == bm_shown) return;
    bm_shown = key;
    VDP_CTRL = 0x8F02;
    vram_w(VRAM_SPRITES);
    if (!bm_on) {
        VDP_DATA = 0; VDP_DATA = 0; VDP_DATA = 0; VDP_DATA = 0;
        return;
    }
    for (uint16_t i = 0; i < bm_n; i++) {
        const uint16_t *e = bm_sat + i * 4;
        uint16_t x = (uint16_t)(e[3] + dx) & 0x1FF;
        if (x == 0) x = 1;                      /* (x 0 is the sprite mask) */
        VDP_DATA = (uint16_t)(e[0] + dy) & 0x3FF;
        VDP_DATA = e[1];
        VDP_DATA = e[2];
        VDP_DATA = x;
    }
}

/* pointer to pack data through the 1MB bank window (blocks never straddle a bank) */
static const uint16_t *pack_ptr(uint32_t off)
{
    uint32_t rom = PACK_ROM + off;
    BANKREG = (uint16_t)(rom >> 20);
    return (const uint16_t *)(0x900000UL + (rom & 0xFFFFFUL));
}

static uint32_t rd32(const uint16_t *p)
{
    return ((uint32_t)p[0] << 16) | p[1];
}

static const uint8_t vdp_regs[19] = {
    0x14,                     /* 80: H int on */
    0x74,                     /* 81: display, V int, DMA, V28 */
    VRAM_PLANE_A / 0x400,
    0xF000 / 0x400,           /* window (unused) */
    VRAM_PLANE_B / 0x2000,
    VRAM_SPRITES / 0x200,
    0x00, 0x00, 0x00, 0x00,
    0x00,                     /* 8A: H int every line */
    0x02,                     /* 8B: cell H scroll, full V scroll */
    0x89,                     /* 8C: H40 + shadow/highlight */
    VRAM_HSCROLL / 0x400,
    0x00, 0x02,
    0x01,                     /* 90: 64x32 planes */
    0x00, 0x00,
};

/* (I18d) the cartridge's SSF2 mapper registers ($A130F1-$A130FF): the 32X hardware manual (4.1, "ROM
   access competition") asks for RV = 1 (DREQ control $A15107 bit 0) while they are written.  RV = 1
   also maps the cartridge at $000000 for the 68000 (not $880000 / $900000) and holds the SH-2s' ROM
   reads until RV = 0 again: RAM code, interrupts off for those few cycles. */
static RAM_CODE __attribute__((noinline)) void mapper_w(uint32_t reg, uint8_t v)
{
    uint16_t sr;
    __asm__ volatile("move.w %%sr,%0\n\tmove.w #0x2700,%%sr" : "=d"(sr));
    volatile uint8_t *dreq = (volatile uint8_t *)0xA15107;
    uint8_t d = *dreq;
    *dreq = (uint8_t)(d | 1);
    *(volatile uint8_t *)reg = v;
    *dreq = (uint8_t)(d & ~1);
    __asm__ volatile("move.w %0,%%sr" : : "d"(sr));
}

volatile uint16_t dbg_stage;
void md_setup(void)
{
    md_pal = VDP_CTRL & 1;
    dbg_stage = 1;
    mapper_w(0xA130F1, 0);          /* SSF2 mapper: SRAM off, window 7 = bank 7 */
    mapper_w(0xA130FF, 7);
    mapper_w(0xA130FD, 6);          /* (I16) window 6 = world bank 0, as the SH-2 assumes
                                                   (cur_bank6): a reset in another world kept its bank */
    for (int i = 0; i < 19; i++)
        VDP_CTRL = 0x8000 | (i << 8) | vdp_regs[i];
    vram_w(0);
    for (uint32_t i = 0; i < 0x8000; i++)
        VDP_DATA = 0;
    cram_w(0);
    for (int i = 0; i < 64; i++)
        VDP_DATA = 0;
    vsram_w(0);
    VDP_DATA = 0;
    VDP_DATA = 0;
    active = 0;
    dbg_stage = 2;
}

/* SH-2 command: load level idx (or clear with 0x7FFF). Interrupts are off meanwhile. */
/* fx 4 (comp levels, performance variant): plane A = BG1, streamed from a grid of 4x4-tile chunks */
static const uint16_t *pf_grid, *pf_vch;
static uint16_t pf_gw, pf_gh, pf_ox0, pf_oy0, pf_valid;
static int16_t pf_tx0, pf_ty0;          /* plane A holds tiles [pf_tx0, +42) x [pf_ty0, +30) */

static RAM_CODE uint16_t pf_cell(int16_t tx, int16_t ty)
{
    if (tx < 0 || ty < 0) return 0;
    uint16_t cx = (uint16_t)tx >> 2, cy = (uint16_t)ty >> 2;
    if (cx >= pf_gw || cy >= pf_gh) return 0;
    uint16_t v = pf_grid[(uint32_t)cy * pf_gw + cx];
    return pf_vch[(uint32_t)v * 16 + (ty & 3) * 4 + (tx & 3)];
}
/* the plane holds rows [ty0 - 1, ty0 + 30) and columns [tx0 - 1, tx0 + 42): one cell of look-ahead on
   every side, so a row or column is in VRAM before it scrolls into view (the 68000 updates the
   scroll registers first, at the start of vblank, and streams afterwards) */
static RAM_CODE void pf_col(int16_t tx, int16_t ty0)
{
    VDP_CTRL = 0x8F80;                      /* auto-increment one plane row (128 bytes) */
    int16_t ty = ty0 - 1, te = ty0 + 30;
    while (ty < te) {
        uint16_t r = ty & 31, n = 32 - r;
        if (n > te - ty) n = te - ty;
        vram_w(VRAM_PLANE_A + r * 128 + (tx & 63) * 2);
        for (uint16_t i = 0; i < n; i++, ty++) VDP_DATA = pf_cell(tx, ty);
    }
}
static RAM_CODE void pf_row(int16_t ty, int16_t tx0)
{
    VDP_CTRL = 0x8F02;
    int16_t tx = tx0 - 1, te = tx0 + 42;
    while (tx < te) {
        uint16_t c = tx & 63, n = 64 - c;
        if (n > te - tx) n = te - tx;
        vram_w(VRAM_PLANE_A + (ty & 31) * 128 + c * 2);
        for (uint16_t i = 0; i < n; i++, tx++) VDP_DATA = pf_cell(tx, ty);
    }
}
static RAM_CODE void pf_update(int16_t gx, int16_t gy)
{
    int16_t tx0 = gx >> 3, ty0 = gy >> 3;
    if (!pf_valid || tx0 - pf_tx0 > 8 || pf_tx0 - tx0 > 8 || ty0 - pf_ty0 > 8 || pf_ty0 - ty0 > 8) {
        for (int16_t ty = ty0 - 1; ty < ty0 + 30; ty++) pf_row(ty, tx0);
        pf_valid = 1;
    } else {
        while (pf_tx0 < tx0) { pf_col(pf_tx0 + 42, pf_ty0); pf_tx0++; }
        while (pf_tx0 > tx0) { pf_tx0--; pf_col(pf_tx0 - 1, pf_ty0); }
        while (pf_ty0 < ty0) { pf_row(pf_ty0 + 30, pf_tx0); pf_ty0++; }
        while (pf_ty0 > ty0) { pf_ty0--; pf_row(pf_ty0 - 1, pf_tx0); }
    }
    pf_tx0 = tx0;
    pf_ty0 = ty0;
}

/* battery SRAM (the game's 2 KB save data): the master SH-2 sends / receives it a word at a time
   through COMM0 (data), COMM2 (its sequence number, 0xFFFF = end) and COMM4 (the receiver's
   acknowledge).  SRAM: byte n at $900001 + 2n with the 32X bank register on bank 2 and $A130F1 = 1
   (works with the SSF2 mapper in ares / PicoDrive / MiSTer).  Interrupts are off here.
   (I18c) MiSTer shows the SRAM over the WHOLE cart $200000-$3FFFFF while it is on - for the SH-2s
   too (their level / front-end banks, the resident SNES pages): the handshake runs with the SRAM
   off, through a copy in 68000 RAM, and the SRAM is on only for the 2 KB burst (a few ms, the SH-2s
   wait for it in their cached loops). */
#define SRAM8(n) (*(volatile uint8_t *)(0x900001UL + 2UL * (n)))
#define SRAM_WORDS 1028
static uint8_t sram_buf[SRAM_WORDS * 2];
static RAM_CODE void sram_on(void)
{
    mapper_w(0xA130F1, 1);
    BANKREG = 2;
}
static RAM_CODE void sram_xfer(uint16_t op)
{
    uint16_t bank = BANKREG;
    COMM(4) = 0;
    if (op != 1) {                          /* load: SRAM -> buffer first */
        sram_on();
        for (uint16_t n = 0; n < SRAM_WORDS * 2; n++) sram_buf[n] = SRAM8(n);
        BANKREG = bank;
        mapper_w(0xA130F1, 0);
    }
    COMM(C_CMD) = 0x9FF0;                   /* ready */
    /* (I18b) never wait forever with interrupts off: the SH-2 gives up after 2 s (sram_wait), so give
       up too after ~4 s of no progress (and leave COMM(C_CMD) alone if the SH-2 has sent something
       new meanwhile) - a lost handshake hung every later command (STUCK in load_level) */
    uint32_t idle = 0;
    if (op == 1) {                          /* save: SH-2 -> buffer, then the buffer -> SRAM */
        uint16_t last = 0;
        for (;;) {
            uint16_t s;
            while ((s = COMM(2)) == last)
                if (++idle > 1500000UL) goto out;
            idle = 0;
            if (s == 0xFFFF || s > SRAM_WORDS) break;
            uint16_t w = COMM(0), n = (uint16_t)((s - 1) * 2);
            sram_buf[n] = (uint8_t)(w >> 8);
            sram_buf[n + 1] = (uint8_t)w;
            COMM(4) = s;
            last = s;
        }
        if (last == SRAM_WORDS) {           /* (only a complete save) */
            sram_on();
            for (uint16_t n = 0; n < SRAM_WORDS * 2; n++) SRAM8(n) = sram_buf[n];
            BANKREG = bank;
            mapper_w(0xA130F1, 0);
        }
    } else {                                /* load: buffer -> SH-2 */
        for (uint16_t i = 0; i < SRAM_WORDS; i++) {
            COMM(0) = (uint16_t)(sram_buf[2 * i] << 8 | sram_buf[2 * i + 1]);
            COMM(2) = (uint16_t)(i + 1);
            while (COMM(4) != (uint16_t)(i + 1))
                if (++idle > 1500000UL) goto out;
            idle = 0;
        }
    }
    COMM(C_CMD) = 0;
    return;
out:
    if (COMM(C_CMD) == 0x9FF0) COMM(C_CMD) = 0;
}

static void load_md(const uint16_t *md);
static void fe_plane(uint16_t i);
static void do_command(uint16_t cmd)
{
    if (cmd == 0x9D00) {                    /* (I13) SH-2 (load_level): display off now, backdrop black - the
                                               32X layer goes off next (the old level's planes showed
                                               alone for a few frames while the new level loaded) */
        VDP_CTRL = 0x8134;
        disp_hold = 0;
        fl_on = 0;
        VDP_CTRL = 0x8F02;
        cram_w(0);
        VDP_DATA = 0;
        COMM(C_CMD) = 0;
        return;
    }
    if ((cmd & 0xFF00) == 0x9E00) {         /* (I11) SH-2: front-end screen n's Genesis plane */
        fe_plane(cmd & 0xFF);
        return;
    }
    if ((cmd & 0xFF00) == 0x9F00) {         /* SH-2: save (1) / load (2) the battery SRAM */
        sram_xfer(cmd & 0xFF);
        return;
    }
    if ((cmd & 0xE100) == 0xA100) {
        /* SH-2: show ROM bank n in cart window 6 (0x300000: the world bank - its SNES page table and
           pages; the 68000 reads nothing there) */
        mapper_w(0xA130FD, (uint8_t)(cmd & 0x3F));
        COMM(C_CMD) = 0;
        return;
    }
    if ((cmd & 0xE000) == 0xA000) {
        /* SH-2: show ROM bank n in cart window 7 (SSF2 mapper; 0x380000 = the level data).  The
           level's plane animations / lamp / weather tables point there: stop them first */
        active = 0;
        anim_n = 0;
        lc_on = 0;
        hv_on = 0;
        gl_on = 0;
        wx_tab = 0;
        mapper_w(0xA130FF, (uint8_t)(cmd & 0xFF));
        COMM(C_CMD) = 0;
        return;
    }
    uint16_t idx = (cmd & 0x7FFF) == 0x7FFF ? 0x7FFF : cmd & 0x1FFF;
    uint16_t variant = (cmd & 0x4000) && idx != 0x7FFF;
    no_ripple = (cmd & 0x2000) && idx != 0x7FFF;     /* (I14: the options menu's RIPPLE OFF) */
    VDP_CTRL = 0x8134;                      /* display off */
    active = 0;
    if (idx == 0x7FFF) {
        /* both planes: (I21) a VDP DMA fill (the CPU loop took ~3 frames the SH-2 waited for at every
           screen change); the last 16 bytes by the CPU (no fill length quirk can reach the sprites) */
        VDP_CTRL = 0x8F01;                  /* auto-increment 1 */
        VDP_CTRL = 0x9300 | (0x7FF0 & 0xFF);
        VDP_CTRL = 0x9400 | (0x7FF0 >> 8);
        VDP_CTRL = 0x9780;                  /* DMA fill */
        VDP_CTRL32 = 0x40000080UL | ((uint32_t)(VRAM_PLANE_A & 0x3FFF) << 16) | (VRAM_PLANE_A >> 14);
        VDP_DATA = 0;
        while (VDP_CTRL & 2)                /* (DMA busy) */
            ;
        VDP_CTRL = 0x8F02;
        vram_w(VRAM_PLANE_A + 0x7FF0);
        for (int i = 0; i < 8; i++)
            VDP_DATA = 0;
        cram_w(0);
        for (int i = 0; i < 64; i++)
            VDP_DATA = 0;
        planes = 0;
        anim_n = 0;
        wx_tab = 0;
        wx_buf_ready = 0;
        lc_on = 0;
        fl_on = 0;
        gl_on = 0;
        bm_lvl = bm_on = 0;
        bm_shown = 0xFFFF;
        bm_write();
    } else {
        const uint16_t *hp = pack_ptr(0);
        uint32_t off_levels = rd32(hp + 6);
        const uint16_t *lv = pack_ptr(off_levels + (uint32_t)idx * 140);
        uint32_t off_md = rd32(lv + (variant ? 64 : 58));
        {   /* weather block (kind 4): CRAM tables for the day fade */
            uint32_t off_fog = rd32(lv + 66), lfl = rd32(lv + 62);
            wx_tab = 0;
            wx_cur = 0;
            wx_buf_ready = 0;
            anim_en = 3;
            anim_blank = 0;
            bg3fade = 0; bg3k = bg3want = 0;
            wx_jungle = 0;
            if (!variant && (lfl & 0x10) && off_fog) {      /* (pack_ptr switches the ROM bank) */
                const uint16_t *wb = pack_ptr(off_fog);
                const uint16_t *keys = 0;
                if (wb[1] == 4) {                           /* jungle: start, end palette; 32 steps */
                    wx_nkey = 2; wx_shift = 5; keys = wb + 8;
                    wx_jungle = 1;
                    bg3fade = wb[7];
                    for (int i = 0; i < 224; i++) bg3tgt[i] = wb[488 + i];
                    if (!bg3fade) {                         /* rain: the lightning flash = key 2 (after the pixels) */
                        const uint16_t *fk = wb + 712 + 257 + wb[0];
                        for (int i = 0; i < 64; i++) wx_ram[128 + i] = fk[i];
                    }
                } else if (wb[1] == 5 || wb[1] == 6) {      /* snow levels: states 0-5; light switches */
                    wx_nkey = wb[4]; wx_shift = wb[5]; keys = wb + 8 + 2 * wb[(wb[1] == 5) ? 3 : 2];
                }
                if (keys && wx_nkey <= 6) {
                    for (uint16_t i = 0; i < wx_nkey * 64; i++) wx_ram[i] = keys[i];
                    wx_tab = wx_ram;
                    if (wb[1] == 4 && !bg3fade) wx_nkey = 3;
                }
            }
        }
        load_md(pack_ptr(off_md));
    }
    VDP_CTRL = (active && (fx == 3 || fx == 4 || fx == 6 || lc_on)) ? 0x8B03 : (active && fx == 5) ? 0x8B00 : 0x8B02;  /* line / full / cell H scroll */
    if (idx != 0x7FFF && active) {          /* a level: on with the SH-2's picture (md_vint) */
        disp_hold = 2;
        hold_f = COMM(C_FRAME);
        VDP_CTRL = 0x8134 | 0x20;           /* (display off, V-int on) */
    } else {
        disp_hold = 0;
        VDP_CTRL = 0x8174;                  /* display on */
    }
    COMM(C_CMD) = 0;
}

/* a plane block (build_viewer md_block): tiles, plane maps, CRAM, scroll / effect parameters; the level's
   extras after them (animated tiles, snow, blizzard, lamp cones, glimmer, beam) */
static void load_md(const uint16_t *md)
{
    {
        uint16_t ntiles = md[0];
        planes = md[1];
        for (int i = 0; i < 6; i++) { scA[i] = md[2 + i]; scB[i] = md[8 + i]; }
        fx = md[14];
        anim_n = md[15];
        uint16_t h = (planes & 4) ? 64 : 32;
        const uint16_t *cram = md + 16;
        const uint16_t *col = md + 16 + 64;
        const uint16_t *tiles = md + 304;
        cram_w(0);
        for (int i = 0; i < 64; i++)
            VDP_DATA = cram[i];
        for (int i = 0; i < 224; i++)
            bg3buf[0][i] = bg3buf[1][i] = bg3day[i] = col[i];
        cur_bg3col = 0xFFFF;
        vram_w(0);
        for (uint32_t i = 0; i < (uint32_t)ntiles * 16; i++)
            VDP_DATA = tiles[i];
        const uint16_t *m = tiles + (uint32_t)ntiles * 16;
        VDP_CTRL = 0x9000 | (h == 64 ? 0x11 : 0x01);
        vram_w(VRAM_PLANE_A);
        for (uint32_t i = 0; i < (uint32_t)h * 64; i++)
            VDP_DATA = (planes & 1) ? *m++ : 0;
        if (h == 32)
            for (uint32_t i = 0; i < 64 * 32; i++)
                VDP_DATA = 0;
        vram_w(VRAM_PLANE_B);
        for (uint32_t i = 0; i < (uint32_t)h * 64; i++)
            VDP_DATA = (planes & 2) ? *m++ : 0;
        anim_data = m;
        altB = altB_on = 0;
        hv_frames = 0;
        hv_on = 0;
        VDP_CTRL = 0x8200 | VRAM_PLANE_A / 0x400;
        const uint16_t *after = anim_data + (uint32_t)anim_n * (1 + 8 * 16);
        if (planes & 8) {
            const uint16_t *alt = after;
            for (int i = 0; i < 6; i++) { scB_alt[i] = alt[i]; scB_main[i] = scB[i]; }
            vram_w(0x8000);
            for (uint32_t i = 0; i < 64 * 32; i++)
                VDP_DATA = alt[6 + i];
            after = alt + 6 + 64 * 32;
            altB = 1;
        }
        if (planes & 16) {
            hv_tile = after[0];
            hv_sway = (const uint8_t *)(after + 2);
            vram_w(0x6000);
            for (uint32_t i = 0; i < 64 * 32; i++)
                VDP_DATA = after[34 + i];
            hv_frames = after + 34 + 64 * 32;
            after = hv_frames + 8 * 1024;
        }
        lc_on = 0;
        fl_on = 0;
        if (planes & 64) {                      /* lamp cones */
            lc_n = after[0] > 16 ? 16 : after[0];
            lc_pivot = after[1];
            lc_pos = after + 4;
            lc_map = lc_pos + 2 * after[0];
            lc_cols = lc_map + 28 * 22;
            lc_swing = lc_cols + 40;
            after = lc_swing + 126;
            lc_have = 0; lc_op = -1; lc_t = 0; lc_d = 0xFFFF;
            lc_on = 1;
        }
        gl_on = 0;
        gl_shown = 0xFFFF;
        if (planes & 128) {                     /* glimmer sprite blocks (tiles came with the planes') */
            gl_n = after[0] > 32 ? 32 : after[0];
            gl_blk = after + 4;
            after = gl_blk + 3 * after[0];
            gl_t = 0x60;
            gl_on = 1;
        }
        bm_lvl = bm_on = 0;
        bm_shown = 0xFFFF;
        if (planes & 32) {                      /* Squawks' beam: sprite tiles + SAT template */
            bm_n = after[0] > 64 ? 64 : after[0];
            uint16_t nt = after[1];
            for (uint16_t i = 0; i < bm_n * 4; i++) bm_sat[i] = after[4 + i];
            const uint16_t *tl = after + 4 + after[0] * 4;
            vram_w((uint32_t)after[2] * 32);
            for (uint32_t i = 0; i < (uint32_t)nt * 16; i++)
                VDP_DATA = tl[i];
            after = tl + (uint32_t)nt * 16;
            bm_lvl = 1;
#ifdef BEAMGRID
            cram_w(0x62);                       /* (test: line 3 colours 1-3 red, green, blue) */
            VDP_DATA = 0x000E; VDP_DATA = 0x00E0; VDP_DATA = 0x0E00;
#endif
        }
        if (planes & 256) {                     /* (I21f) flicker: CRAM B (64 words, lines 0-2 used) */
            fl_a = cram;
            fl_b = after;
            after += 64;
            fl_ph = fl_k = 0;
            fl_sel = 0xFFFF;
            fl_on = 1;
        }
        bm_ax = BM_AX0; bm_ay = BM_AY0;
        bm_write();                             /* no sprites (the SAT's first entry ends the list) */
        VDP_CTRL = 0x8400 | VRAM_PLANE_B / 0x2000;
        VDP_CTRL = altB ? 0x8C81 : 0x8C89;      /* snow: no S/H (priority orders the planes); (I18) lamps: S/H on,
                                                   the wall and the cone shadowed, the lanterns' cells high priority */
        if (fx == 4 || fx == 5) {
            pf_gw = m[0]; pf_gh = m[1]; pf_ox0 = m[2]; pf_oy0 = m[3];
            pf_grid = m + 6;
            pf_vch = pf_grid + (uint32_t)pf_gw * pf_gh;
            pf_valid = 0;
            anim_n = 0;
        }
        active = 1;
        last_frame = COMM(C_FRAME) ^ 0xFFFF;
        hud_shown = 1;                          /* first comp_hud with the HUD off clears it */
    }
}

/* (I11) front-end screen i's Genesis plane (the SH-2 shows its ROM bank in window 7 first): Cranky's
   clouds (BG2) on plane B, scrolled by C_CAMX / C_CAMY like a level's; the colours fade with the screen's
   brightness through the weather keyframes (C_HUD 0x4000 | 1 << 8 | k: black -> the palette, k / 16) */
static void fe_plane(uint16_t i)
{
    disp_hold = 0;
    VDP_CTRL = 0x8134;                      /* display off */
    active = 0;
    planes = 0;
    anim_n = 0;
    wx_tab = 0;
    wx_buf_ready = 0;
    lc_on = 0;
    fl_on = 0;
    gl_on = 0;
    hv_on = 0;
    bm_lvl = bm_on = 0;
    bg3fade = 0;
    anim_en = 3;
    anim_blank = 0;
    const uint16_t *hp = pack_ptr(0);
    uint32_t off_fe = rd32(hp + 24);
    uint32_t n = rd32(pack_ptr(off_fe));
    uint32_t off_md = rd32(pack_ptr(off_fe + 4 + n * 8 + (uint32_t)i * 4));
    if (off_md) {
        const uint16_t *md = pack_ptr(off_md);
        for (int k = 0; k < 64; k++) { wx_ram[k] = 0; wx_ram[64 + k] = md[16 + k]; }
        load_md(md);
        cram_w(0);                              /* (black until the first brightness message) */
        for (int k = 0; k < 64; k++)
            VDP_DATA = 0;
        wx_nkey = 2;
        wx_shift = 4;
        wx_cur = 0;
        wx_tab = wx_ram;
        VDP_CTRL = 0x8C81;                      /* (no shadow / highlight: the plane as drawn) */
    }
    VDP_CTRL = 0x8B02;                          /* cell H scroll (one value: BG2's) */
    VDP_CTRL = 0x8174;                          /* display on */
    COMM(C_CMD) = 0;
}

/* 3-button pad -> S A C B R L D U (active high) */
static uint16_t read_pad(void)
{
    uint8_t hi, lo;
    IO_DATA1 = 0x40;
    __asm__ volatile("nop; nop");
    hi = IO_DATA1;
    IO_DATA1 = 0x00;
    __asm__ volatile("nop; nop");
    lo = IO_DATA1;
    IO_DATA1 = 0x40;
    uint16_t v = (hi & 0x3F) | ((lo & 0x30) << 2);
    return (~v) & 0xFF;
}

/* s = {mode, shift}; the vertical value also has s[2] = sub, s[3] = clamp (scA + 2: ym, ys, sub, clamp) */
static uint16_t sval(uint16_t cam, const uint16_t *s, int vert)
{
    uint16_t v = cam;
    if (vert) {
        v -= s[2];
        if (s[3] != 0xFFFF && v > s[3]) v = s[3];
    }
    switch (s[0]) {
    case 0: return v >> s[1];
    case 1: return v + (v >> 1);
    case 2: return v << 1;
    case 4: return (v >> s[1]) & 0xFF;     /* SNES scroll register written as 8 bits (wraps) */
    case 5: return (uint16_t)((v << 1) + s[1]);   /* 2 x + offset (Gang-Plank Galleon BG3) */
    default: return s[1];
    }
}

/* (I21) camx = the 32X view's left edge = the game's camera - dd (dd = the SH-2's spr_xoff, sent in
   C_FRAME bits 9-15): the SNES draws a plane at H = f(game camera) on its 256 columns, which the 32X shows
   dd columns to the right - so the Genesis H is f(camx + dd) - dd.  Before I21 the planes used f(camx):
   off by dd - f(dd) (24 px for a 1/4-speed layer), which only showed where a plane must line up with
   the 32X layer - Gang-Plank Galleon's island (a sprite) over its shadow (plane A).  Lamp levels keep
   the old formula (their frame counter needs all 16 bits: dd = 0). */
/* (I21b) dd = 0 in every level: the I20 scroll from the view camera (with the live d in every level the
   backgrounds slid into place at each level start as d eased from 0 to 32: user).
   (I21c) Galleon boss (fx 6): dd = 32, a CONSTANT - the planes still follow only the view camera (with
   the live d the whole sea and sky slid whenever the view offset changed at the playfield's edges:
   user), the island's band (H 0 on the SNES: fixed on screen) sits 32 px right, and the SH-2 draws the
   island sprite 32 px right of the SNES picture instead of d (sprites.c spr_fixed_x) */
static void update_scroll(uint16_t camx, uint16_t camy, uint16_t dd)
{
    uint16_t ha = 0, va = 0, hb = 0, vb = 0;
    uint16_t cx = (uint16_t)(camx + dd);
    if (planes & 1) { ha = (uint16_t)(sval(cx, scA, 0) - (fx == 2 ? 0 : dd)); va = sval(camy, scA + 2, 1) + 1; }
    if (planes & 2) { hb = (uint16_t)(sval(cx, scB, 0) - dd); vb = sval(camy, scB + 2, 1) + 1; }
    uint16_t a = (uint16_t)(-ha) & 0x3FF;
    if (lc_on) {
        /* plane A = the cones in level coordinates (64 x 64 cells); the nearest lamp's swing */
        int16_t cy = (int16_t)(camy + 1), best = 0x7FFF, oy = 0;
        for (uint16_t i = 0; i < lc_n; i++) {
            int16_t d = (int16_t)(lc_pos[2 * i] * 8 + 88 - camx - 160);
            if (d < 0) d = -d;
            if (d < best) { best = d; oy = (int16_t)(lc_pos[2 * i + 1] * 8); }
        }
        /* the SNES swing (CODE_80B891) from the game's frame counter (C_FRAME = $28 - 1 here):
           SNES HOFS at table line j (0 = apex = origin + lc_pivot) += (W >> 4) + j * W / 256 */
        uint16_t f2 = (uint16_t)(last_frame << 1), p = f2 & 0x7F;
        if (p & 0x40) p ^= 0x7F;
        int16_t v = (int16_t)(lc_swing[p] >> 1);
        if (f2 & 0x80) v = (int16_t)~v;
        int16_t W = (int16_t)(v >> 1);
        int32_t k = -W;
        int32_t acc = (int16_t)(cy - oy - (int16_t)lc_pivot);
        __asm__("muls.w %1,%0" : "+d"(acc) : "d"(k));
        acc -= (int32_t)(W >> 4) << 8;
        /* (I18) the checkerboard's other half every vblank by a 1-line V scroll (md_vint) - it was
           the game frame's parity in the H scroll: with frame skip (2 frames a picture) the cone kept
           one half for seconds */
        uint16_t ha0 = (uint16_t)(-camx), b = (uint16_t)(-hb) & 0x3FF;
        lc_vy = (uint16_t)cy;
        VDP_CTRL = 0x8F02;
        vsram_w(0);
        VDP_DATA = (uint16_t)(cy + lc_ph) & 0x3FF;
        VDP_DATA = vb;
        vram_w(VRAM_HSCROLL);
        for (int l = 0; l < 224; l++) {
            VDP_DATA = (ha0 + (int16_t)(acc >> 8)) & 0x3FF;
            VDP_DATA = b;
            acc += k;
        }
        return;
    }
    if (fx == 5) {
        /* plane A: BG1 (streamed); plane B: its own scroll (full-screen scroll mode) */
        int16_t gx = (int16_t)(camx - pf_ox0), gy = (int16_t)(camy + 1 - pf_oy0);
        VDP_CTRL = 0x8F02;
        vram_w(VRAM_HSCROLL);
        VDP_DATA = (uint16_t)(-gx) & 0x3FF;
        VDP_DATA = (uint16_t)(-hb) & 0x3FF;
        vsram_w(0);
        VDP_DATA = (uint16_t)gy & 0x3FF;
        VDP_DATA = vb;
        pf_update(gx, gy);
        return;
    }
    if (fx == 4) {
        /* plane A: BG1 (streamed), ripple x per line; plane B: BG2 perspective (as fx 3) */
        int16_t gx = (int16_t)(camx - pf_ox0), gy = (int16_t)(camy + 1 - pf_oy0);
        static uint16_t hl[80];
        uint16_t A = (uint16_t)(cx - 0x280) >> 1;
        uint16_t nA = (uint16_t)-A;
        uint16_t step = nA + (nA >> 1);
        uint16_t acc = (uint16_t)(A << 8 | A >> 8);
        for (int d = 0; d < 80; d++) { hl[d] = (uint16_t)(-(acc >> 8) + dd) & 0x3FF; acc += step; }
        uint16_t a0 = (uint16_t)(-gx) & 0x3FF, a1 = (uint16_t)(-gx - 1) & 0x3FF;
        uint16_t ph = last_frame;
        VDP_CTRL = 0x8F02;
        vsram_w(0);                              /* vertical first: it applies to the whole frame */
        VDP_DATA = (uint16_t)gy & 0x3FF;
        VDP_DATA = vb;
        vram_w(VRAM_HSCROLL);
        for (int l = 0; l < 224; l++) {
            uint16_t q = (l - ph) & 63;          /* underwater ripple: 22 on, 11 off, 10 on, 21 off */
            VDP_DATA = (!no_ripple && (q < 22 || (q >= 33 && q < 43))) ? a1 : a0;
            VDP_DATA = l < 80 ? hl[l] : l < 144 ? (dd & 0x3FF) : hl[223 - l];
        }
        pf_update(gx, gy);
        return;
    }
    if (fx == 6) {
        /* (I11) Gang-Plank Galleon: the sky / sea (BG2, plane A) per line (CODE_809A6E -> $7F0200, HDMA
           ch1 -> BG2HOFS): 8.8 value (integer in the LOW byte) from camx / 4 at line 0 and line 160,
           + ~(camx + camx / 16) / 256 per line towards the horizon (64 lines each); lines 64-96 (the
           island, its shadow) 0; the table ends at line 128 (lines below keep line 127's) */
        static uint16_t hl[64];
        uint16_t v = cx >> 2, d = (uint16_t)~((cx >> 4) + cx);
        for (int k = 0; k < 64; k++) {
            hl[k] = (uint16_t)(-(v & 0xFF) + dd) & 0x3FF;
            uint16_t w = (uint16_t)((v << 8 | v >> 8) + d);
            v = (uint16_t)(w << 8 | w >> 8);
        }
        uint16_t bb = (uint16_t)(-hb) & 0x3FF;
        VDP_CTRL = 0x8F02;
        vram_w(VRAM_HSCROLL);
        for (int l = 0; l < 64; l++) { VDP_DATA = hl[l]; VDP_DATA = bb; }
        for (int l = 64; l < 97; l++) { VDP_DATA = dd & 0x3FF; VDP_DATA = bb; }
        for (int l = 97; l < 128; l++) { VDP_DATA = hl[160 - l]; VDP_DATA = bb; }
        for (int l = 128; l < 224; l++) { VDP_DATA = hl[33]; VDP_DATA = bb; }
        vsram_w(0);
        VDP_DATA = va;
        VDP_DATA = vb;
        return;
    }
    if (fx == 3) {
        /* underwater BG2 (plane A) perspective (CODE_819741, HDMA ch2 -> BG2HOFS): 8.8 accumulator,
           start (camx - $280) / 2 at the bottom / top line, step -1.5 x that per line towards the
           middle; lines 80-143 scroll 0.  Line scroll mode, plane A/B word pairs. */
        static uint16_t hl[80];
        uint16_t A = (uint16_t)(cx - 0x280) >> 1;
        uint16_t nA = (uint16_t)-A;
        uint16_t step = nA + (nA >> 1);
        uint16_t acc = (uint16_t)(A << 8 | A >> 8);
        for (int d = 0; d < 80; d++) { hl[d] = (uint16_t)(-(acc >> 8) + dd) & 0x3FF; acc += step; }
        uint16_t bb = (uint16_t)(-hb) & 0x3FF;
        VDP_CTRL = 0x8F02;
        vram_w(VRAM_HSCROLL);
        for (int l = 0; l < 80; l++) { VDP_DATA = hl[l]; VDP_DATA = bb; }
        for (int l = 80; l < 144; l++) { VDP_DATA = dd & 0x3FF; VDP_DATA = bb; }
        for (int l = 144; l < 224; l++) { VDP_DATA = hl[223 - l]; VDP_DATA = bb; }
        vsram_w(0);
        VDP_DATA = va;
        VDP_DATA = vb;
        return;
    }
    VDP_CTRL = 0x8F20;
    vram_w(VRAM_HSCROLL);
    for (int r = 0; r < 28; r++)
        VDP_DATA = a;
    vram_w(VRAM_HSCROLL + 2);
    if (fx == 1) {
        /* jungle BG3: 4 parallax bands (CODE_809AD4) */
        uint16_t x2 = cx << 1;
        uint16_t b0 = (uint16_t)(-((x2 >> 4) & 0xFF) + dd) & 0x3FF;
        uint16_t b1 = (uint16_t)(-(((x2 + cx) >> 4) & 0xFF) + dd) & 0x3FF;
        uint16_t b2 = (uint16_t)(-(((x2 + x2) >> 4) & 0xFF) + dd) & 0x3FF;
        uint16_t b3 = (uint16_t)(-(((x2 + x2 + cx) >> 4) & 0xFF) + dd) & 0x3FF;
        for (int r = 0; r < 9; r++) VDP_DATA = b0;
        VDP_DATA = b1;
        VDP_DATA = b2;
        for (int r = 11; r < 28; r++) VDP_DATA = b3;
    } else {
        uint16_t b = (uint16_t)(-hb) & 0x3FF;
        for (int r = 0; r < 28; r++)
            VDP_DATA = b;
    }
    VDP_CTRL = 0x8F02;
    vsram_w(0);
    VDP_DATA = va;
    VDP_DATA = vb;
}

/* comp levels (fx 2): the HUD lives on plane A (font tiles 2 + 2*char, 8x16) so the SH-2s don't spend
   framebuffer writes on it.  FPS = presented frames (COMM C_FRAME) per 60 vblanks. */
static const char hud_chars[] = "0123456789FPSMX:%/ Y";

static RAM_CODE void hud_put(uint16_t row, uint16_t col, const char *s)
{
    for (uint16_t half = 0; half < 2; half++) {
        vram_w(VRAM_PLANE_A + ((row + half) * 64 + col) * 2);
        for (const char *q = s; *q; q++) {
            uint16_t gi = 0;
            while (hud_chars[gi] && hud_chars[gi] != *q) gi++;
            VDP_DATA = 0x8000 | (2 + gi * 2 + half);
        }
    }
}

static RAM_CODE void num(char *p, uint16_t v, int digits)
{
    for (int i = digits - 1; i >= 0; i--) { p[i] = '0' + (v % 10); v /= 10; }
}

static uint8_t hud_val[3];
static RAM_CODE void comp_hud(void)
{
    uint16_t h = COMM(C_HUD);
    uint16_t sel = (h >> 12) & 3;
    if (sel < 3) hud_val[sel] = h & 0xFF;
    if (!(h & 0x8000)) {
        if (hud_shown) {                       /* restore: border cells black, the rest clear */
            hud_shown = 0;
            for (uint16_t r = 0; r < 4; r++) {
                vram_w(VRAM_PLANE_A + r * 128);
                for (uint16_t c = 0; c < 20; c++) VDP_DATA = c < 4 ? 1 : 0;
            }
        }
        return;
    }
    hud_shown = 1;
    char l1[] = "FPS 00 M 00% S 00%";
    char l2[] = "X 00000 Y 00000";
    num(l1 + 4, hud_val[0] > 99 ? 99 : hud_val[0], 2);
    num(l1 + 9, hud_val[1] > 99 ? 99 : hud_val[1], 2);
    num(l1 + 15, hud_val[2] > 99 ? 99 : hud_val[2], 2);
    num(l2 + 2, COMM(C_CAMX), 5);
    num(l2 + 10, COMM(C_CAMY), 5);
    hud_put(0, 0, l1);
    hud_put(2, 0, l2);
}

static RAM_CODE uint16_t gl_rand(void)
{
    uint16_t r = gl_rng;
    r ^= r << 7;
    r ^= r >> 9;
    r ^= r << 8;
    return gl_rng = r;
}
static RAM_CODE void gl_frame(void)
{
    static const uint8_t scale[8] = { 0, 8, 18, 27, 31, 27, 18, 8 };
    uint16_t vis = 0;
    if (gl_t == 0) {
        gl_t = 0x60 + (gl_rand() & 0x3F);
        gl_base = (int16_t)(gl_rand() & 0x60) - 0x40;
    } else
        gl_t--;
    if (!(gl_t & ~0x1F)) {
        if ((gl_t & 7) == 0) {
            gl_x = (uint16_t)((gl_rand() & 0x3F) + gl_base);
            gl_y = (uint16_t)(((((gl_x - gl_base) ^ 0xFF) & 0xFF) >> 1) - 0x50);
        } else {                            /* the fade: 27-31 every frame, 18 every other frame */
            uint16_t sc = scale[gl_t & 7];
            vis = sc >= 27 || (sc >= 18 && (gl_t & 2));
        }
    }
#ifdef GLDBG
    vis = 1;
#endif
    uint16_t ph = gl_t & 1;                 /* dither phase: the pattern moves 1 px every frame */
    uint16_t key = vis ? (uint16_t)(((gl_x + ph) & 0xFF) | (gl_y & 0xFF) << 8) : 0xFFFF;
    if (key == gl_shown) return;
    gl_shown = key;
    VDP_CTRL = 0x8F02;
    vram_w(VRAM_SPRITES);
    if (!vis) {
        VDP_DATA = 0; VDP_DATA = 0; VDP_DATA = 0; VDP_DATA = 0;
        return;
    }
    /* screen pixel (x, y) shows glow map ((x + gx) & 255, (y + gy + 1) & 255) (BG3 scroll) */
    uint16_t gx = (gl_x + ph) & 0xFF, gy = gl_y & 0xFF, n = 0;
    static uint16_t sat[64 * 4];
    for (uint16_t i = 0; i < gl_n; i++) {
        const uint16_t *b = gl_blk + i * 3;
        int16_t sy = (int16_t)((b[1] - gy - 1 + 32) & 255) - 32;
        int16_t sx = (int16_t)((b[0] - gx + 32) & 255) - 32;
        if (sy >= 224) continue;
        for (int16_t x = sx; x < 320 && n < 64; x += 256) {
            uint16_t *e = sat + n * 4;
            e[0] = (uint16_t)(sy + 128); e[1] = 0x0F00; e[2] = 0xE000 | b[2];   /* (high priority: the planes are) */ e[3] = (uint16_t)(x + 128);
            n++;
        }
    }
    if (!n) { VDP_DATA = 0; VDP_DATA = 0; VDP_DATA = 0; VDP_DATA = 0; return; }
    for (uint16_t i = 0; i < n; i++) {
        VDP_DATA = sat[i * 4];
        VDP_DATA = sat[i * 4 + 1] | (i + 1 < n ? i + 1 : 0);
        VDP_DATA = sat[i * 4 + 2];
        VDP_DATA = sat[i * 4 + 3];
    }
}

/* lamp cones: flicker colours (CODE_80B800, as the SH-2's lamp_frame) and the plane B streaming:
   8 cell rows per frame of one lamp's cone written (or cleared when it left the view) */
static RAM_CODE void lc_colours(void)
{
    uint16_t f = COMM(C_FRAME), e = f >> 2;
    uint16_t t1 = e & 0x7F, t2 = ((e + f) >> 1) & 0x7F;
    if (t1 & 0x40) t1 ^= 0x7F;
    if (t2 & 0x40) t2 ^= 0x7F;
    uint16_t d = (t1 + t2) >> 5;
    if ((f & 1) && ((t1 + t2) & 16)) d++;
    if (d > 3) d = 3;
    if (d != lc_d) {
        lc_d = d;
        VDP_CTRL = 0x8F02;
        cram_w(0x62);                       /* line 3, colours 1-10 */
        for (int i = 0; i < 10; i++) VDP_DATA = lc_cols[d * 10 + i];
    }
}
static RAM_CODE void lc_frame(void)
{
    VDP_CTRL = 0x8F02;
    if (lc_op < 0) {
        int16_t vx = (int16_t)(COMM(C_CAMX) >> 3);
        uint16_t want = 0;
        for (uint16_t i = 0; i < lc_n; i++) {
            int16_t cx = (int16_t)lc_pos[2 * i];
            /* (I11) at most 63 cells wide: two cones 64 cells apart share their plane cells (the
               plane is 64 x 64); one would be written over the other while it is still in view */
            if (cx + 22 > vx - 1 && cx < vx + 41) want |= 1u << i;
        }
        for (uint16_t i = 0; i < lc_n && lc_op < 0; i++)
            if ((lc_have & ~want) >> i & 1) { lc_op = i; lc_clear = 1; lc_row = 0; }
        for (uint16_t i = 0; i < lc_n && lc_op < 0; i++)
            if ((want & ~lc_have) >> i & 1) { lc_op = i; lc_clear = 0; lc_row = 0; }
        if (lc_op < 0) return;
    }
    uint16_t cx = lc_pos[2 * lc_op], cy = lc_pos[2 * lc_op + 1];
    for (uint16_t n = 0; n < 8 && lc_row < 28; n++, lc_row++) {
        const uint16_t *m = lc_map + lc_row * 22;
        uint16_t c = cx & 63, r = (cy + lc_row) & 63, i = 0;
        while (i < 22) {
            uint16_t run = 64 - c;
            if (run > 22 - i) run = 22 - i;
            vram_w(VRAM_PLANE_A + r * 128 + c * 2);
            for (uint16_t j = 0; j < run; j++, i++) VDP_DATA = lc_clear ? 0 : m[i];
            c = 0;
        }
    }
    if (lc_row >= 28) {
        if (lc_clear) {
            lc_have &= ~(1u << lc_op);
            /* (I11) the cells just cleared may be another loaded cone's too (cones 42..63 cells apart
               meet at the plane's wrap): that cone is written again - it lost a chunk ("missing part
               of the beam" once it came into view) */
            for (uint16_t j = 0; j < lc_n; j++) {
                if (!(lc_have >> j & 1)) continue;
                uint16_t dc = (uint16_t)(lc_pos[2 * j] - cx) & 63, dr = (uint16_t)(lc_pos[2 * j + 1] - cy) & 63;
                if ((dc < 22 || dc > 64 - 22) && (dr < 28 || dr > 64 - 28)) lc_have &= ~(1u << j);
            }
        } else lc_have |= 1u << lc_op;
        lc_op = -1;
    }
}

/* (I21c) the 68000's load for the pause menu's STATS line: lines spent in md_vint / md_idle work (VDP V
   counter) + an estimate per H interrupt, per 60 frames, in % of 262 lines; sent to the SH-2 bit by bit in
   C_PAD bits 15 (data) / 14 (first bit) - 7 bits MSB first, one a vblank (main.c pad_store) */
static uint32_t ld_acc;
static uint16_t ld_frames, ld_pct, ld_bit;
static uint16_t ld_hints;
static inline uint16_t ld_lines(uint8_t v0)
{
    uint8_t v1 = VDP_VCNT;
    return (uint8_t)(v1 - v0);
}
__attribute__((interrupt_handler)) RAM_CODE void md_vint(void)
{
    uint8_t ld_v0 = VDP_VCNT;
    dbg_v++;
    if (bg3_swap) { uint16_t *t = bg3col; bg3col = bg3nxt; bg3nxt = t; bg3_swap = 0; }
    for (int i = 0; i < 8; i++) dbg_comm[i] = COMM(i * 2);
    if (active) {
        /* (I21g) the camera / picture this vblank shows, read once (update_scroll below uses the same) */
        const uint16_t vcx = COMM(C_CAMX), vcy = COMM(C_CAMY), vcf = COMM(C_FRAME);
        if (fl_on) {
            /* (I21f) flicker: CRAM lines 0-2 from table A or B, first thing.  (I21g) The checkerboard is
               in plane B's coordinates, so a plane scroll by an odd number of pixels swaps its halves
               on screen: the table now follows the vblank count XOR the parity of plane B's H + V
               scroll (the one this frame shows) - every screen pixel alternates every vblank whatever
               the 32X's picture rate (frame drops made pixels stay 2 vblanks on one colour: a 15 Hz
               flash) */
            if (vcf != last_frame && (planes & 2))
                fl_k = (sval(vcx, scB, 0) + sval(vcy, scB + 2, 1) + 1) & 1;
            uint16_t sel = (uint16_t)((fl_ph ^= 1) ^ fl_k);
            if (sel != fl_sel) {
                const uint16_t *c = sel ? fl_b : fl_a;
                fl_sel = sel;
                VDP_CTRL = 0x8F02;
                cram_w(0);
                for (int i = 0; i < 48; i += 8) {
                    VDP_DATA = c[i]; VDP_DATA = c[i + 1]; VDP_DATA = c[i + 2]; VDP_DATA = c[i + 3];
                    VDP_DATA = c[i + 4]; VDP_DATA = c[i + 5]; VDP_DATA = c[i + 6]; VDP_DATA = c[i + 7];
                }
            }
        }
        if (wx_buf_ready) {                     /* CRAM first, while the beam is surely in vblank */
            wx_buf_ready = 0;
            VDP_CTRL = 0x8F02;
            cram_w(0);
            for (int i = 0; i < 64; i++) VDP_DATA = wx_buf[i];
#ifdef BEAMGRID
            cram_w(0x62);
            VDP_DATA = 0x000E; VDP_DATA = 0x00E0; VDP_DATA = 0x0E00;
#endif
            if (lc_on) lc_d = 0xFFFF;           /* (the lamp colours again) */
        }
        if (lc_on) lc_colours();
        uint16_t cf = vcf;
        uint16_t f = cf;
        if (f != last_frame) {
            last_frame = f;
            update_scroll(vcx, vcy, fx == 6 && !lc_on ? 32 : 0);
            if (bm_lvl) bm_write();
        }
        if (hv_on) {                            /* blizzard (CODE_808B1F): frames, wind drift + sway */
            uint16_t f = COMM(C_FRAME);         /* (the 32X draws the same flakes over BG1) */
            VDP_CTRL = 0x8F02;
            if (f & 1) {
                const uint32_t *src = (const uint32_t *)(hv_frames + ((f & 0xE) >> 1) * 1024);
                volatile uint32_t *d = (volatile uint32_t *)0xC00000;
                vram_w((uint32_t)hv_tile * 32);
                for (int i = 0; i < 512; i += 8) {
                    d[0] = src[i]; d[0] = src[i + 1]; d[0] = src[i + 2]; d[0] = src[i + 3];
                    d[0] = src[i + 4]; d[0] = src[i + 5]; d[0] = src[i + 6]; d[0] = src[i + 7];
                }
            }
            uint16_t t = f & 0x7F;
            if (t & 0x40) t ^= 0x7F;
            uint16_t sw = hv_sway[t] >> 1;
            if (f & 0x80) sw = ~sw;
            uint16_t H = (uint16_t)(-((sw + f) & 0xFF)) & 0x3FF, V = (f & 1) ^ 1;
            VDP_CTRL = 0x8F20;                  /* plane B's word of each cell row (cell H scroll) */
            vram_w(VRAM_HSCROLL + 2);
            for (int r = 0; r < 28; r++)
                VDP_DATA = H;
            VDP_CTRL = 0x8F02;
            vsram_w(2);
            VDP_DATA = V;
        }
        if (lc_on) {
            lc_ph ^= 1;                         /* (I18) the cone's other checkerboard half */
            VDP_CTRL = 0x8F02;
            vsram_w(0);
            VDP_DATA = (uint16_t)(lc_vy + lc_ph) & 0x3FF;
            lc_frame();
        }
        if (fx == 2)
            comp_hud();
        /* per-vblank animations run at the game's 60 Hz: on a 50 Hz (PAL) console every 5th vblank
           runs them twice */
        uint16_t nt = 1;
        if (md_pal && ++md_palph == 5) {
            md_palph = 0;
            nt = 2;
        }
        for (; nt; nt--) {
        if (gl_on)
            gl_frame();
        /* weather snow (CODE_8089A0): new tile frame every other frame */
        /* weather snow (CODE_8089A0): group 1 ($5E00) new frame every other frame, group 2 ($5F00,
           tile word bit 15) every frame; a disabled group is blanked once */
        if (anim_n) {
            uint16_t af = ++anim_frame;
            uint16_t fa = (af & 0xE) >> 1, fb = af & 7;
            const uint16_t *a = anim_data;
            for (uint16_t k = 0; k < anim_n; k++, a += 1 + 8 * 16) {
                uint16_t g = (a[0] & 0x8000) ? 2 : 1;
                if (anim_en & g) {
                    if (g == 1 && !(af & 1)) continue;
                    vram_w((uint32_t)(a[0] & 0x7FFF) * 32);
                    const uint16_t *src = a + 1 + (g == 1 ? fa : fb) * 16;
                    for (int i = 0; i < 16; i++)
                        VDP_DATA = src[i];
                } else if (!(anim_blank & g)) {
                    vram_w((uint32_t)(a[0] & 0x7FFF) * 32);
                    for (int i = 0; i < 16; i++)
                        VDP_DATA = 0;
                }
            }
            anim_blank = (anim_blank | (uint16_t)(~anim_en & 3)) & (uint16_t)(~anim_en & 3);
        }
        }
        if (wx_tab) {
            uint16_t w = COMM(C_HUD);
            if (bm_lvl && (w & 0xC000) == 0x4000) {
                /* Torchlight: the beam's apex (y 0xFF: no beam) -> palette key 1 / 0 (instant) */
                uint16_t on = (w & 0xFF) != 0xFF;
                if (on) { bm_ax = (int16_t)((w >> 7) & 0x7E); bm_ay = (int16_t)(w & 0xFF); }
                w = on ? 0x4000 | 1 << 11 | 1 << 8 : 0x4000;
                if (on == bm_on) bm_write();
            }
            if ((w & 0xC000) == 0x4000 && w != wx_cur) {
                int16_t k = (int16_t)(w & 0x3F);
                uint16_t from = (w >> 11) & 7, to = (w >> 8) & 7;
                if (from >= wx_nkey) from = 0;
                if (to >= wx_nkey) to = 0;
                wx_cur = w;
                /* snow tiles: $5E00 from weather 2; BG1's $5F00 snow (2nd plane B map) once the fade
                   into 4 is done (the mountains on plane B are the flat sky grey by then) */
                uint16_t lo = from < to ? from : to;
                /* jungle: treeline once day breaks.  (I14: Ropey has 3 keys - night, day, the lightning
                   flash - and the old `wx_nkey == 2` test sent it down the snow branch: the treeline map
                   never came up after the storm) */
                uint16_t b2 = wx_jungle ? (to == 1 && k > 0) :
                              (from >= 4 && to >= 4) || (lo == 3 && to >= 4 && k == (1 << wx_shift));
                anim_en = (to >= 2 ? 1 : 0) | (b2 && altB ? 2 : 0);
                if (altB && b2 != altB_on) {
                    altB_on = b2;
                    if (!hv_on) VDP_CTRL = 0x8400 | (b2 ? 0x8000 : VRAM_PLANE_B) / 0x2000;
                    for (int i = 0; i < 6; i++) scB[i] = b2 ? scB_alt[i] : scB_main[i];
                    last_frame ^= 0xFFFF;               /* rescroll */
                }
                uint16_t hw = hv_frames && wx_nkey == 6 && to == 5;
                if (hw != hv_on) {                      /* blizzard: plane B shows its map, in front of
                                                           plane A's BG2 trees */
                    hv_on = hw;
                    VDP_CTRL = 0x8400 | (hw ? 0x6000 : altB_on ? 0x8000 : VRAM_PLANE_B) / 0x2000;
                    last_frame ^= 0xFFFF;
                }
                const uint16_t *na = wx_tab + from * 64, *nb = wx_tab + to * 64;
                for (int i = 0; i < 64; i++) {
                    int16_t n = (int16_t)na[i], d = (int16_t)nb[i];
                    uint16_t c = 0;
                    for (int16_t sh = 1; sh <= 9; sh += 4) {
                        int16_t a = (n >> sh) & 7, b = (d >> sh) & 7;
                        int32_t m = (int16_t)(b - a);
                        __asm__("muls.w %1,%0" : "+d"(m) : "d"(k));   /* 16x16 (no libgcc) */
                        int16_t v = a + (int16_t)(m >> wx_shift);
                        c |= (uint16_t)v << sh;
                    }
                    wx_buf[i] = c;
                }
                wx_buf_ready = 1;                       /* (written at the start of the next vblank:
                                                           CRAM writes during the display show as dots) */
                cur_bg3col = 0xFFFF;
                if (bm_lvl) { bm_on = to == 1; bm_write(); }   /* Torchlight: the beam once Squawks is out */
                if (bg3fade) bg3want = (uint16_t)k;      /* (recomputed in md_idle, outside the interrupt) */
            }
        }
        if (fx == 1 && cur_bg3col != bg3col[0]) {
            cur_bg3col = bg3col[0];
            VDP_CTRL = 0x8F02;
            cram_w(2);
            VDP_DATA = cur_bg3col;
        }
    }
    if (disp_hold) {
        uint16_t f = COMM(C_FRAME);
        if (f != hold_f) {
            hold_f = f;
            if (--disp_hold == 0) VDP_CTRL = 0x8174;
        }
    }
    hcount = 0;
    /* (I17) high byte = this vblank's sample number: the master SH-2 keeps every vblank's pad in order
       (main.c pad_take), so logic frames run back to back under frame skip each get their own sample -
       a press that lasted one vblank was missed when two logic frames read the same register value */
    static uint8_t pad_seq;
    pad_seq++;
    uint16_t ldb = (uint16_t)((ld_pct >> (6 - ld_bit)) & 1) << 15 | (ld_bit == 0 ? 0x4000 : 0);
    if (++ld_bit == 7) ld_bit = 0;
#ifdef PADTEST
    COMM(C_PAD) = (uint16_t)((uint16_t)(pad_seq & 0x3F) << 8 | ldb | ((pad_seq & 31) == 0 ? 0x18 : 0x08));   /* (test: B for one vblank in 32) */
#else
    COMM(C_PAD) = (uint16_t)((uint16_t)(pad_seq & 0x3F) << 8 | ldb | read_pad());
#endif
    ld_acc += (uint32_t)ld_lines(ld_v0) * 16 + (ld_hints * 3 >> 1);   /* (an H int ~ 3/32 line) */
    ld_hints = 0;
    if (++ld_frames == 60) {
        {   uint32_t q = ld_acc;                     /* (divu: 32 / 16 bits, no libgcc) */
            __asm__("divu.w %1,%0" : "+d"(q) : "d"((uint16_t)(262 * 16 * 60 / 100)));
            ld_pct = (uint16_t)q; }
        if (ld_pct > 99) ld_pct = 99;
        ld_acc = 0;
        ld_frames = 0;
    }
}

/* jungle gradient: count H-ints instead of reading VCNT (which may already show the next line,
   depending on the interrupt latency -> colour bands jumped a line between frames) */
__attribute__((interrupt_handler)) RAM_CODE void md_hint(void)
{
    uint16_t l = ++hcount;                 /* the line about to be drawn */
    ld_hints++;
    if (fx != 1)
        return;
    /* resync against VCNT (= the line just ended, or already the next one): if interrupts were lost
       or doubled (seen on MiSTer: the haze band picked colours from the black end of the table) */
    uint8_t v = VDP_VCNT;
    if ((uint8_t)(l - v) > 1 && v < 224) {
        l = v + 1;
        hcount = l;
    }
    if (l >= 224)
        return;
    uint16_t c = bg3col[l];
    if (c != cur_bg3col) {
        cur_bg3col = c;
        cram_w(2);
        VDP_DATA = c;
    }
}

RAM_CODE void md_idle(void)
{
    dbg_stage = 3;
    __asm__ volatile("move.w #0x2300,%sr");
    for (;;) {
        __asm__ volatile("stop #0x2300");
        uint16_t c = COMM(C_CMD);
        dbg_c = c;
        if (active && bg3fade && bg3k != bg3want && !bg3_swap) {
            uint8_t ld_v0 = VDP_VCNT;
            uint16_t *dst = bg3nxt;         /* (not the global: md_vint may swap meanwhile) */
            int16_t k = (int16_t)bg3want;
            bg3k = bg3want;
            for (int l = 0; l < 224; l++) {
                uint16_t d = bg3day[l], t = bg3tgt[l], v = 0;
                for (int16_t sh = 1; sh <= 9; sh += 4) {
                    int16_t a = (d >> sh) & 7;
                    int32_t m = (int16_t)(((t >> sh) & 7) - a);
                    __asm__("muls.w %1,%0" : "+d"(m) : "d"(k));
                    v |= (uint16_t)(a + (int16_t)(m >> 5)) << sh;
                }
                dst[l] = v;         /* (I14: the H-int read a half-written table - the haze bands jumped) */
            }
            bg3_swap = 1;           /* (md_vint swaps the tables at the next vblank) */
            ld_acc += (uint32_t)ld_lines(ld_v0) * 16;
        }
        if (c & 0x8000) {
            __asm__ volatile("move.w #0x2700,%sr");
            do_command(c);
            __asm__ volatile("move.w #0x2300,%sr");
        }
    }
}
