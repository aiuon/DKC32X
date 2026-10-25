/* ---- the front-end screens of the complete game (Rare / Nintendo logos, Cranky's intro, title, (c),
   file select, player select): drawn live from the game's PPU state, like a small SNES PPU
   (included by main.c; ROM code).

   - the screen: the pack's table (tools/build_viewer.py, from tools/capture.py fe_capture) by the
     game's NMI routine ($1C) and BG mode -> that screen's VRAM as captured (64 KB, banked): the
     character data, and the maps the mirror does not have
   - the maps: sprites.c mirrors every VRAM write to words 7000-7FFF into fe_vm (the slave's tile
     cache memory, lent while no level is on screen) - the file select's text is written from the
     save data while the screen is set up
   - per-line registers: the game's HDMA tables are run every frame for BG mode, main / sub screen,
     colour math, the mode 7 matrix and BG1 scroll (the logos switch modes within a frame)
   - modes 0/1/3 (tiles, both priorities, BG3 priority), 5 (hi-res: every other pixel), 7 (affine)
   - colour math (add subscreen): exact sums in CRAM 128-254 when no sprites are shown (player
     select's highlight), else "main pixel black -> the subscreen's" (Cranky's intro cross-fade)
   - brightness (INIDISP) and the backdrop (CGRAM 0) through the palette
   Redrawn when anything it was drawn from changes (signature per frame buffer); with sprites
   (TM bit 4) the bands under the sprites are redrawn every frame and the sprites drawn on top. */

typedef struct { uint16_t nmi; uint8_t mode, bank; uint32_t off_vram; } fescr_t;
/* CRAM 128: always black - the sides outside the SNES picture (index 0 is the game's backdrop colour,
   cream behind the credits); colour sums use 129-254 */
#define FE_BORDER 128
#define FE_BORDER4 0x80808080u
#ifndef FE_U32                            /* (the pack is big-endian: the host test build swaps) */
#define FE_U32(x) (x)
#define FE_U16(x) (x)
#endif
#ifndef FE_HI8                            /* a 16-bit read of bytes p[0] p[1]: p[0] = the high byte (big-endian) */
#define FE_HI8(w) ((w) >> 8)
#define FE_LO8(w) ((w) & 255)
#endif
#define FE_LE16(p) (FE_LO8(*(const uint16_t *)(p)) << 8 | FE_HI8(*(const uint16_t *)(p)))   /* (VRAM words: little-endian) */

/* per-line registers: 8-bit BGMODE TM TS CGWSEL CGADSUB, 16-bit M7A-D, BG1 h / v scroll (mode 7: 13 bit) */
enum { L_MODE, L_TM, L_TS, L_CGW, L_CGA, NL8 };
enum { L_M7A, L_M7B, L_M7C, L_M7D, L_H1, L_V1, NL16 };
typedef struct {
    int idx;                              /* the screen shown (-1: none) */
    const uint8_t *vr;                    /* its captured VRAM (byte address) */
    uint32_t sig[2];                      /* what each frame buffer was drawn from */
    uint32_t palsum;                      /* the palette uploaded (sum), ~0 = upload */
    uint32_t npair;                       /* colour sums in use (CRAM 128 + k) */
    int half, obj;                        /* colour math halves; sprites on the main screen */
    uint32_t hlatch;                      /* (HDMA write-twice latch) */
    uint32_t lsum;                        /* the per-line registers' sum (fe_find) */
    int m7;                               /* a mode 7 line */
    int32_t m7x0, m7x1, m7y0, m7y1;       /* mode 7: the map pixels holding non-blank tiles (fe_setup) */
    int pfull;                            /* the colour sums ran out (fe_pair) */
    uint32_t pfull_f;                     /* (the frame of the last reset: not again within 60) */
    uint32_t sdark[8];                    /* subscreen colours too dark to add (r + g + b < 8) */
    uint32_t nlines;                      /* (lines drawn last frame: test builds) */
    int ynext;                            /* the first line not drawn for lack of time */
    int32_t m7w;                          /* mode 7: the SDRAM copy's width in tiles (0: none, use the ROM) */
    uint32_t nfr;                         /* pictures since fe_setup */
    int32_t xa, xb;                       /* fe_line: the SNES x range drawn (multiples of 8) */
    uint32_t cval[2][7];                  /* per buffer: line y's clean copy (FE_CLEAN) is valid */
    uint16_t rkey[4];                     /* fe_rowflag: the BG map / char base / bpp its caches are for */
    int perline;                          /* BG2-4 scroll in the line signatures (modes 1 / 3) */
    int hold;                             /* pictures held back unfinished in a row (I5) */
    uint32_t ogen;                        /* (I5) frames for FE_LGEN / FE_DGEN (0: not set up) */
    int subm;                             /* (I5) main screen = the backdrop only, the subscreen added: the
                                             subscreen drawn as the main one, the backdrop added by CRAM */
    uint32_t mdark[4];                    /* BG colours dark now (r + g + b < 8): sprites + colour math show the
                                             subscreen where the main colour is dark (~ main + sub) */
    int gplane;                           /* (I11) BG2 is the Genesis plane B (build_viewer fe_bg2_plane): not
                                             drawn here, its pixels left transparent; 0x1000 | its BG2 rows'
                                             write generations when its map was last checked */
    uint32_t gsum;                        /* (I11) the captured BG2 map's sum (the plane's) */
    uint32_t grg;                         /* (I11) BG2SC and its map rows' write generations at the last check */
} fe_state_t;
typedef char fe_state_fits[sizeof(fe_state_t) <= 256 ? 1 : -1];
/* each frame buffer keeps a clean copy (BGs only, no sprites) of lines FE_CY0.. in its free space
   after the picture (the canvas lines 0-223 and one spare): the restore under the last sprites is a
   copy, not a redraw from the cartridge (~25k cycles a line with three layers) */
#define FE_CY0 12
#define FE_CN 200
#define FE_CLEAN_OFF ((FB_BASE + (SCREEN_H + 1) * CV_P + 15) & ~15)
#define FE_CLEAN(y) ((volatile uint32_t *)(FB8 + FE_CLEAN_OFF + ((y) - FE_CY0) * 256))
typedef char fe_clean_fits[FE_CLEAN_OFF + FE_CN * 256 <= 0x20000 ? 1 : -1];

/* work memory in tc_sdata (16 KB, the slave's tile cache: unused while no level is shown) */
#define FE_VM     ((uint16_t *)((uint8_t *)tc_sdata))              /* 4096 words: VRAM 7000-7FFF, then fe_trk_t */
#define FE        ((fe_state_t *)UNCACHED((uint8_t *)tc_sdata + 16128))   /* (<= 256 bytes) */   /* (valid while lent; uncached) */
#define FE_EXP    ((uint32_t *)((uint8_t *)tc_sdata + 8320))       /* 256 x 2: bit planes -> byte lanes */
#define FE_PKEY   ((uint16_t *)((uint8_t *)tc_sdata + 10368))      /* colour sums: 127 x (main | sub << 8) */
#define FE_PHASH  ((uint8_t *)tc_sdata + 10624)                    /* 512: hash -> sum + 1 (0 = empty) */
#define FE_LB     ((uint8_t *)tc_sdata + 11136)                    /* 272: main line, sub line, layer ids */
#define FE_SB     ((uint8_t *)tc_sdata + 11408)
#define FE_SRC    ((uint8_t *)tc_sdata + 11680)
#define FE_L8     ((uint8_t (*)[SCREEN_H])((uint8_t *)tc_sdata + 11952))     /* [NL8][224] */
#define FE_L16    ((int16_t (*)[SCREEN_H])((uint8_t *)tc_sdata + 13072))     /* [NL16][224] */
#define FE_SET    ((uint32_t (*)[8])((uint8_t *)tc_sdata + 15760))           /* [NL8 + NL16][8] lines written */
#define FE_MENT   ((uint16_t (*)[34])((uint8_t *)map_mem + 2048))            /* [4][34] a line's map entries */
#define FE_SCRL   ((uint16_t (*)[8])((uint8_t *)map_mem + 2336))            /* [3][8]: BG scroll used, last, before (fe_steady) */
#define FE_HOFS(i) (FE_SCRL[0][i])
#define FE_VOFS(i) (FE_SCRL[0][4 + (i)])
#define FE_M7MAP  ((uint8_t *)map_mem + 2560)                                /* 2 KB: mode 7 box map copy */
#define FE_M7CHR  ((uint8_t *)map_mem + 4608)                                /* 160 x 64: its characters */
#define FE_LUSED  ((uint32_t (*)[4])((uint8_t *)map_mem + 4608))              /* [224][4] (no mode 7 copy: FE->m7w == 0)
                                                                                 main BG colours whose black-ness decided a
                                                                                 line's colour math (sprites shown) */
/* (I5, non-mode-7 screens: after FE_LUSED) per line the colours (& 127) its "brighter of the two"
   pixels compared; per colour the frame its brightness level last changed; per buffer and line the
   frame it was drawn */
#define FE_OVF    ((uint32_t (*)[4])((uint8_t *)map_mem + 8192))               /* [224][4] */
#define FE_LGEN   ((uint16_t *)((uint8_t *)map_mem + 11776))                   /* [128] */
#define FE_DGEN   ((uint16_t (*)[SCREEN_H])((uint8_t *)map_mem + 12032))       /* [2][224] */
#define FE_LVL    ((uint8_t *)map_mem + 12928)                                 /* [256] the levels */
#define FE_CHST   ((uint8_t (*)[256])((uint8_t *)map_mem + 14848))           /* [4][256]: 2 bits a char, 0 = not known, 1 blank, 2 not */
#define FE_ROWC   ((uint16_t (*)[64])((uint8_t *)map_mem + 15872))           /* [2][64]: BG2 / BG3 map tile rows: vgen << 2 | 2 | not empty */
/* (I11) per buffer and map row of the VRAM mirror (32 words): the columns written since its last finished
   picture (sprites.c fe_dirty) - map_mem's upper half, after Torchlight's beam tables (20480 - 22272) */
#define FE_DIRTY  ((uint32_t *)((uint8_t *)map_mem + 22528))                  /* [2][128] */
/* (I12) per line the colours (0-255) whose darkness (FE->sdark) decided its colour math (no sprites shown):
   a colour lit for the first time redraws only the lines that use it - the Rare logo's wireframe lights a
   colour or two each frame: every line was redrawn every frame (dsig), the logo ran at ~10 fps in ares */
#define FE_DUSED  ((uint32_t (*)[8])((uint8_t *)map_mem + 23552))              /* [224][8] (-> 30720) */
#define FE_DHC    ((uint32_t *)((uint8_t *)map_mem + 30720))                    /* [224] this picture's (-> 31616) */
/* (I12) colour math per picture: each colour pair's result (key m | s << 8, result << 16 (0x100: the main
   colour stays), bit 25: the main colour's darkness looked at, bit 26: "brighter of the two") - the Nintendo
   logo's smoke over the logo is a new pair almost every pixel; and the colours' brightness (r + g + b) */
#define FE_PRC    ((uint32_t *)((uint8_t *)map_mem + 31616))                    /* [128] (-> 32128) */
#define FE_BRT    ((uint8_t *)map_mem + 32128)                                  /* [256] (-> 32384) */
static uint32_t fe_dh(int y, const uint32_t *sd)
{
    const uint32_t *u = FE_DUSED[y];
    uint32_t w = 0;
    for (int k = 0; k < 8; k++) w = (w ^ (u[k] & sd[k])) * 0x9E3779B1u;
    return w & ~1u;                       /* (bit 0: FE_LSC2 / FE_LSCA's "| 1") */
}
#define fe_idx     FE->idx
#define fe_vr      FE->vr
#define fe_sig     FE->sig
#define fe_palsum  FE->palsum
#define fe_npair   FE->npair
#define fe_half    FE->half
#define fe_obj     FE->obj
#define fe_ppu     (FE_TRK->ppu)
#define fe_vmask   (FE_TRK->vmask)
#define fe_vgen    (FE_TRK->vgen)

#define FE_VM_UNSET 0xFFFFu                /* (a mirror word not written since fe_lend) */
/* lend / take back the mirror memory (main.c: the slave is idle, no level is drawn) */
#ifdef FEDBG
static uint32_t fedbg_full, fedbg_part, fedbg_copy, fedbg_setup;   /* (test: per picture / total) */
static uint32_t fedbg_tline, fedbg_tsig;  /* (test: FRT ticks in fe_line, in the signature pass) */
static uint32_t fedbg_md, fedbg_small, fedbg_n, fedbg_def, fedbg_h[6];
#ifndef FEHOST
static uint16_t fedbg_tp, fedbg_tk[6];    /* (test: FRT at the last present's end; phase marks this picture) */
static uint32_t fedbg_vp;
#define FEDBG_MARK(k) do { fedbg_tk[k] = frt_read(); } while (0)
#else
#define FEDBG_MARK(k) ((void)0)
#endif
#else
#define FEDBG_MARK(k) ((void)0)
#endif
extern uint32_t *fe_dirty;
static void fe_lend(void)
{
    if (fe_vm) return;
    fe_dirty = 0;                         /* (no screen set up yet: fe_setup) */
    uint32_t *t = (uint32_t *)UNCACHED(FE_VM + 4096);
    for (uint32_t i = 0; i < sizeof(fe_trk_t) / 4; i++) t[i] = 0;
    {   /* words not written since: "unknown" (the captured VRAM) - a block the game writes only
           part of must not show the tile cache's old bytes in the rest (the ending speech's text rows) */
        uint32_t *v = (uint32_t *)UNCACHED(FE_VM);
        for (uint32_t i = 0; i < 2048; i++) v[i] = FE_VM_UNSET * 0x10001u;
    }
    FE->idx = -1;
    fe_vm = UNCACHED(FE_VM);
}
/* (sprites.c, in the middle of a game frame: a map write while not lent) the game's screen is dark
   (its INIDISP shadow $51A) - it is setting up a new screen in the frame it went dark (the hoard ->
   the credits): lend now, once the slave is idle, so this frame's map writes are mirrored too */
#ifdef FEDBGL
uint32_t fe_dbgl[4] __attribute__((used));   /* (test: calls, dark, slave busy, lent) */
#define FE_DBGL(i) (fe_dbgl[i]++)
#else
#define FE_DBGL(i) ((void)0)
#endif
static void fe_lend_early(void)
{
    uint32_t sd = game_wram16(0x51A) & 0xFF;
    FE_DBGL(0);
    if (!(sd & 0x80) && (sd & 15)) return;   /* (a lit screen: a level drawing its maps) */
    FE_DBGL(1);
    uint32_t v0 = vcount;
    while (*UNCACHED(&s_job_now) && vcount - v0 < 3)
        ;
    if (*UNCACHED(&s_job_now)) { FE_DBGL(2); return; }
    FE_DBGL(3);
    fe_lend();
}
static void fe_take(void)
{
    fe_vm = 0;
    fe_dirty = 0;
}

static inline uint32_t fe_vword(uint32_t a)
{
    a &= 0x7FFF;
    if (a >= 0x7000 && (fe_vmask & bit32[(a - 0x7000) >> 8])) {
        uint32_t w = UNCACHED(FE_VM)[a - 0x7000];
        if (w != FE_VM_UNSET) return w;
    }
    return fe_vr[a * 2] | fe_vr[a * 2 + 1] << 8;
}

static uint32_t fe_lines_regs(void);
static void fe_steady(int init);
/* (I11) a sum of the BG2 map: as captured (cap) or as the game has it now (the mirror over the capture) -
   the Genesis plane shows the captured map: a screen whose clouds' map differs draws them itself */
static uint32_t fe_bg2_sum(int cap)
{
    uint32_t sc = game_ppu(0x2108), base = (sc & 0xFC) << 8, nw = 0x400u << ((sc & 1) + ((sc >> 1) & 1)), s = 0;
    for (uint32_t k = 0; k < nw; k++) {
        uint32_t a = (base + k) & 0x7FFF;
        uint32_t w = cap ? (uint32_t)(fe_vr[a * 2] | fe_vr[a * 2 + 1] << 8) : fe_vword(a);
        s = s * 31 + w;
    }
    return s;
}
static void fe_setup(int i)
{
#ifdef FEDBG
    fedbg_setup++;
#endif
    const fescr_t *e = (const fescr_t *)(PK(uint32_t, hdr->off_fe) + 1) + i;
    PATHLOG(6, i);
    render_state_reset();
    set_bank(e->bank);
    UNCACHED(&shared_state)->epoch++;
    fe_vr = PACK + FE_U32(e->off_vram);
    {   /* (the state lives in lent memory: start from zero) */
        uint32_t *z = (uint32_t *)FE;
        for (uint32_t k = 0; k < sizeof(fe_state_t) / 4; k++) z[k] = 0;
    }
    FE->xb = 256;
    fe_vr = PACK + FE_U32(e->off_vram);
    fe_idx = i;
    {   /* bit planes -> one bit per byte lane (lane 0 = leftmost pixel, big-endian words) */
        uint32_t *x = FE_EXP;
        for (uint32_t b = 0; b < 256; b++) {
            uint32_t h = 0, l = 0;
            for (int k = 0; k < 4; k++) {
                h |= ((b >> (7 - k)) & 1) << (24 - 8 * k);
                l |= ((b >> (3 - k)) & 1) << (24 - 8 * k);
            }
            x[b * 2] = h; x[b * 2 + 1] = l;
        }
    }
    {   /* mode 7 (the Rare logo): the box of the map's non-blank tiles - fe_m7 draws only the part of a
           line that falls in it (the logo is a few tiles in a 128 x 128 map) */
        uint8_t *blank = FE_LB;                     /* (scratch: 256 flags) */
        for (int t = 0; t < 256; t++) {
            uint32_t any = 0;
            for (int k = 0; k < 64 && !any; k++) any = fe_vr[(t * 64 + k) * 2 + 1];
            blank[t] = !any;
        }
        int32_t x0 = 1024, x1 = 0, y0 = 1024, y1 = 0;
        for (int ty = 0; ty < 128; ty++)
            for (int tx = 0; tx < 128; tx++)
                if (!blank[fe_vr[(ty * 128 + tx) * 2]]) {
                    if (tx * 8 < x0) x0 = tx * 8;
                    if (tx * 8 + 8 > x1) x1 = tx * 8 + 8;
                    if (ty * 8 < y0) y0 = ty * 8;
                    if (ty * 8 + 8 > y1) y1 = ty * 8 + 8;
                }
        FE->m7x0 = x0; FE->m7x1 = x1; FE->m7y0 = y0; FE->m7y1 = y1;
        /* ... and a copy of that part of the map and its characters in SDRAM (ROM reads per pixel are
           slow): FE_M7MAP = box tiles (char index remapped), FE_M7CHR = 64 bytes per char */
        FE->m7w = 0;
        if (x1 > x0) {
            int32_t w = (x1 - x0) >> 3, h = (y1 - y0) >> 3;
            uint8_t *remap = FE_SB;                     /* (scratch: char -> copy index + 1) */
            for (int t = 0; t < 256; t++) remap[t] = 0;
            int n = 0, ok = w * h <= 2048;
            for (int ty = 0; ty < h && ok; ty++)
                for (int tx = 0; tx < w && ok; tx++) {
                    uint32_t t = fe_vr[((y0 / 8 + ty) * 128 + x0 / 8 + tx) * 2];
                    if (!remap[t]) {
                        if (n >= 160) { ok = 0; break; }
                        remap[t] = (uint8_t)++n;
                        for (int k = 0; k < 64; k++) FE_M7CHR[(n - 1) * 64 + k] = fe_vr[(t * 64 + k) * 2 + 1];
                    }
                    FE_M7MAP[ty * w + tx] = (uint8_t)(remap[t] - 1);
                }
            if (ok) FE->m7w = w;
        }
    }
    {   /* 68000: clear the Genesis planes - or (I11) this screen's BG2 on plane B (in this bank, shown now) */
        const uint32_t *t = PK(uint32_t, hdr->off_fe);
        uint32_t n = FE_U32(t[0]);
        uint32_t md = FE_U32(((const uint32_t *)((const fescr_t *)(t + 1) + n))[i]);
        FE->gplane = md != 0;
        FE->gsum = md ? fe_bg2_sum(1) : 0;
        COMM(C_CMD) = md ? 0x9E00 | (uint16_t)i : 0x8000 | 0x7FFF;
    }
    while (COMM(C_CMD) & 0x8000)
        ;
    pending_pal = 0;
    spr_pal_dirty = 0;
    cv_x0par = 0;                         /* (I11: no 1-pixel shift - present's camx is the plane's scroll) */
    ripple = 0;
    sft_show = 0;
    fe_sig[0] = fe_sig[1] = 0xFFFFFFFF;
    fe_palsum = 0xFFFFFFFF;
    FE->pfull = 1;                        /* (fe_frame: a new colour sum table, all lines drawn) */
    FE->pfull_f = game_stats.frames - 60;
    spr_ck_attach(0);                     /* (map_mem's lower half: FE_LSIG; load_level attaches it again) */
    /* (H5: no flips here - the buffers still hold old pictures (mode 7 leaves its odd lines stale)
       and would be shown with a new line table; fe_frame's first two pictures draw everything,
       sides included) */
    spr_clear_marks(0);
    spr_clear_marks(1);
    {   extern const uint16_t *volatile spr_prio_mask; *UNCACHED(&spr_prio_mask) = 0; }   /* (no level's mask) */
    for (int i = 0; i < 8; i++) FE->sdark[i] = 0xFFFFFFFFu;   /* (dark until seen lit: fe_frame) */
    cvb[0].valid = cvb[1].valid = 0;
    cvb[0].hud = cvb[1].hud = (trect_t){ 0, 0, 0, 0 };
    disp_pending = 1;                     /* (the 32X layer on with the first picture) */
    if (!FE->m7w) for (int y = 0; y < SCREEN_H; y++) FE_LUSED[y][0] = FE_LUSED[y][1] = FE_LUSED[y][2] = FE_LUSED[y][3] = 0;
    for (int y = 0; y < SCREEN_H; y++) for (int k = 0; k < 8; k++) FE_DUSED[y][k] = 0;
    fe_steady(1);
    FE->lsum = fe_lines_regs();           /* (the state was cleared: the per-line registers again) */
    for (int i = 0; i < 256; i++) FE_DIRTY[i] = 0;
    fe_dirty = FE_DIRTY;                  /* (I11: the mirror's writes from now on, per buffer) */
}
typedef char fe_state_fits[sizeof(fe_state_t) <= 256 ? 1 : -1];

/* ---- per-line registers from the HDMA tables */
static void fe_hdma_wr(void *ctx, int y, uint32_t reg, uint32_t v)
{
    (void)ctx;
    int k;
    y = y > 0 ? y - 1 : 0;                /* (our line 0 = SNES scanline 1: line 0 is never shown) */
    switch (reg) {
    case 0x2105: k = L_MODE; break;
    case 0x212C: k = L_TM; break;
    case 0x212D: k = L_TS; break;
    case 0x2130: k = L_CGW; break;
    case 0x2131: k = L_CGA; break;
    case 0x211B: case 0x211C: case 0x211D: case 0x211E: case 0x210D: case 0x210E: {
        uint32_t w = v << 8 | (FE->hlatch & 0xFF);
        FE->hlatch = v;
        k = reg == 0x210D ? L_H1 : reg == 0x210E ? L_V1 : (int)(reg - 0x211B);
        FE_L16[k][y] = (int16_t)w;
        FE_SET[NL8 + k][y >> 5] |= bit32[y & 31];
        return;
    }
    default: return;
    }
    FE_L8[k][y] = (uint8_t)v;
    FE_SET[k][y >> 5] |= bit32[y & 31];
}
/* the BG scroll drawn: as written, except a value that jumps back to the one of the time before (the
   intro's TNT shake: 2 px every frame) - the whole screen would be redrawn at every picture (H8) */
static void fe_steady(int init)
{
    const uint16_t *raw = (const uint16_t *)&fe_ppu;
    uint16_t (*sc)[8] = FE_SCRL;
    for (int i = 0; i < 8; i++) {
        uint32_t r = raw[i];
        /* (I12) BG2 - BG4 as they were while a picture is held unfinished (out of time): the lines drawn for
           it stay right - the Nintendo logo's smoke jumps twice every 9 frames, a held picture was drawn
           over and over (~2 fps); it now shows the smoke as it was when it began */
        if (!init && FE->hold && (i & 3)) { sc[2][i] = sc[1][i]; sc[1][i] = (uint16_t)r; continue; }
        if (init || r == sc[1][i] || r != sc[2][i]) sc[0][i] = (uint16_t)r;
        sc[2][i] = init ? (uint16_t)r : sc[1][i];
        sc[1][i] = (uint16_t)r;
    }
}

static uint32_t fe_lines_regs(void)
{
    fe_steady(0);
    static const uint16_t r8[NL8] = { 0x2105, 0x212C, 0x212D, 0x2130, 0x2131 };
    for (int k = 0; k < NL8 + NL16; k++) for (int i = 0; i < 8; i++) FE_SET[k][i] = 0;
    FE->hlatch = 0;
    game_hdma_run(fe_hdma_wr, 0, SCREEN_H + 1);
    uint32_t s = 0;
    /* (I11) 32 lines at a time: a block without HDMA writes is the value before it (the bit tests and the
       per-line sums were ~0.15 vblank a picture); the sum over whole words */
    for (int k = 0; k < NL8; k++) {
        uint32_t v = game_ppu(r8[k]);
        uint8_t *row = FE_L8[k];
        for (int w = 0; w < SCREEN_H / 32; w++, row += 32) {
            uint32_t set = FE_SET[k][w];
            if (!set) { uint32_t q = v * 0x01010101u; uint32_t *r4 = (uint32_t *)row; for (int i = 0; i < 8; i++) r4[i] = q; continue; }
            for (int i = 0; i < 32; i++) {
                if (set & bit32[i]) v = row[i];
                row[i] = (uint8_t)v;
            }
        }
        const uint32_t *r4 = (const uint32_t *)FE_L8[k];
        for (int i = 0; i < SCREEN_H / 4; i++) s = s * 31 + r4[i];
    }
    int m7 = 0;
    for (int y = 0; y < SCREEN_H; y++) m7 |= (FE_L8[L_MODE][y] & 7) == 7;
    FE->m7 = m7;
    for (int k = 0; k < NL16; k++) {
        int32_t v = k < 4 ? fe_ppu.m7[k] : k == L_H1 ? (m7 ? fe_ppu.m7h : FE_HOFS(0)) : (m7 ? fe_ppu.m7v : FE_VOFS(0));
        int16_t *row = FE_L16[k];
        for (int w = 0; w < SCREEN_H / 32; w++, row += 32) {
            uint32_t set = FE_SET[NL8 + k][w];
            if (!set) { uint32_t q = (uint16_t)v * 0x10001u; uint32_t *r4 = (uint32_t *)row; for (int i = 0; i < 16; i++) r4[i] = q; continue; }
            for (int i = 0; i < 32; i++) {
                if (set & bit32[i]) v = row[i];
                row[i] = (int16_t)v;
            }
        }
        if (m7 || k >= L_H1) {            /* (M7A-D double as the multiplier: only mode 7 draws them) */
            const uint32_t *r4 = (const uint32_t *)FE_L16[k];
            for (int i = 0; i < SCREEN_H / 2; i++) s = s * 31 + r4[i];
        }
    }
    return s;
}

/* the front-end screen the game shows (-1: none in the table): its NMI routine and BG mode (the
   value left after the frame's HDMA - what the capture saw); runs the HDMA tables (fe_frame uses them) */
static int fe_find(void)
{
    if (!hdr->off_fe) return -1;
    FE->lsum = fe_lines_regs();
    const uint32_t *t = PK(uint32_t, hdr->off_fe);
    const fescr_t *e = (const fescr_t *)(t + 1);
    uint32_t nmi = game_wram16(0x1C), mode = FE_L8[L_MODE][SCREEN_H - 1] & 7, n = FE_U32(t[0]);
    if (game_in_main_loop()) {
        /* a main-loop screen that is no level (the credits, entrance 5E): by the entrance $3E */
        uint32_t ent = game_wram16(0x3E) & 0xFF;
        for (uint32_t i = 0; i < n; i++)
            if ((e[i].mode & 0x80) && FE_U16(e[i].nmi) == ent) return (int)i;
        return -1;
    }
    for (uint32_t i = 0; i < n; i++)
        if (FE_U16(e[i].nmi) == nmi && e[i].mode == mode) return (int)i;
    for (uint32_t i = 0; i < n; i++)          /* (a mode the capture did not see: the same screen) */
        if (FE_U16(e[i].nmi) == nmi && !(e[i].mode & 0x80)) return (int)i;
    return -1;
}

/* (main.c's step loop) still the screen shown? - cheap: no HDMA run ($1C, or the entrance) */
static int fe_same(void)
{
    int i = fe_idx;
    if (i < 0 || !hdr->off_fe) return 0;
    const fescr_t *e = (const fescr_t *)(PK(uint32_t, hdr->off_fe) + 1) + i;
    if (e->mode & 0x80) return game_in_main_loop() && (game_wram16(0x3E) & 0xFF) == FE_U16(e->nmi);
    return !game_in_main_loop() && game_wram16(0x1C) == FE_U16(e->nmi);
}

/* an entrance-keyed screen's world bank (window 6; 0 = none) - the entrance's, or 0 */
static uint32_t fe_ent_bank6(uint32_t ent)
{
    if (!hdr->off_fe) return 0;
    const uint32_t *t = PK(uint32_t, hdr->off_fe);
    const fescr_t *e = (const fescr_t *)(t + 1);
    for (uint32_t i = 0, n = FE_U32(t[0]); i < n; i++)
        if ((e[i].mode & 0x80) && FE_U16(e[i].nmi) == ent) return e[i].mode & 0x3F;
    return 0;
}

#ifndef FE_WAIT_VBL                      /* (the next vblank, no flip; the host test: nothing) */
#define FE_WAIT_VBL() do { while (REG16(VDP_FBCTL) & VDP_VBLK) ; while (!(REG16(VDP_FBCTL) & VDP_VBLK)) ; } while (0)
#endif
#ifndef FE_W
#define FE_W(x) (x)                       /* (big-endian: lane 0 = the first byte; the host test swaps) */
#endif
/* (I12) a word's pixels moved n bits towards later / earlier addresses (memory order: the host test is
   little-endian) */
#ifdef FEHOST
#define FE_LATER(x, n) ((x) << (n))
#define FE_EARLIER(x, n) ((x) >> (n))
#else
#define FE_LATER(x, n) ((x) >> (n))
#define FE_EARLIER(x, n) ((x) << (n))
#endif
/* 0xFF in each byte lane that is not 0 */
static inline uint32_t fe_lanemask(uint32_t x)
{
    x |= (x >> 4) & 0x0F0F0F0Fu;
    x |= (x >> 2) & 0x33333333u;
    x |= (x >> 1) & 0x55555555u;
    x &= 0x01010101u;
    return (x << 8) - x;
}

/* does BG li's map tile row ty (0-63) hold any non-blank character?  Cached per vgen (map writes);
   the character blank states per BG (char base, bpp) are fixed: the captured VRAM.  (BG2 / BG3 only:
   a moving layer - the speech's clouds - then only redraws the lines it shows on) */
/* layer li's character blank states (FE_CHST: 2 bits a char - 01 blank, 10 not, 00 not known yet), for its
   current map / character base and depth; the BG2 / BG3 row cache with them */
static const uint8_t fe_cs_blank[4] = { 0x01, 0x04, 0x10, 0x40 }, fe_cs_known[4] = { 0x03, 0x0C, 0x30, 0xC0 };
static uint8_t *fe_chst(int li, int bpp)
{
    uint32_t sc = game_ppu(0x2107 + li), nba = game_ppu(0x210B + (li >> 1));
    uint32_t key = (sc << 8 | (((li & 1) ? nba >> 4 : nba) & 15) << 4 | (bpp & 7) | ((bpp & 0x80) ? 8 : 0)) & 0xFFFF;
    uint8_t *cs = FE_CHST[li];
    if (FE->rkey[li] != key) {
        FE->rkey[li] = (uint16_t)key;
        for (int i = 0; i < 256; i++) cs[i] = 0;
        if (li >= 1 && li <= 2) { uint16_t *rc = FE_ROWC[li - 1]; for (int i = 0; i < 64; i++) rc[i] = 0; }
    }
    return cs;
}
static uint32_t fe_rowflag(int li, uint32_t ty, int bpp)
{
    uint32_t sc = game_ppu(0x2107 + li), nba = game_ppu(0x210B + (li >> 1));
    uint8_t *cs = fe_chst(li, bpp);
    uint16_t *rc = FE_ROWC[li - 1];
    uint32_t vg = fe_vgen, stamp = (vg & 0x3FFF) << 2 | 2;
    ty &= 63;
    if ((rc[ty] & ~1u) == stamp) return rc[ty] & 1;
    uint32_t mapw = (sc & 0xFC) << 8, msz = sc & 3;
    uint32_t chw = ((li & 1) ? nba >> 4 : nba & 15) << 12, tb = (bpp & 15) * 8;
    uint32_t mrow = mapw + (ty & 31) * 32;
    if ((msz & 2) && (ty & 32)) mrow += (msz & 1) ? 0x800 : 0x400;
    const uint8_t *vr = fe_vr;
    uint32_t vm = fe_vmask, any = 0;
    const uint16_t *vmw = UNCACHED(FE_VM);
    for (uint32_t tx = 0; tx < ((msz & 1) ? 64u : 32u) && !any; tx++) {
        uint32_t a = (mrow + (tx & 31) + ((tx & 32) ? 0x400 : 0)) & 0x7FFF;
        uint32_t e = a >= 0x7000 && (vm & bit32[(a - 0x7000) >> 8]) ? vmw[a - 0x7000] : FE_VM_UNSET;
        if (e == FE_VM_UNSET) e = FE_LE16(vr + a * 2);
        uint32_t c = e & 0x3FF, st = (cs[c >> 2] >> ((c & 3) * 2)) & 3;
        if (st != 2 && (bpp & 0x80)) {   /* (hi-res: a 16-pixel tile = chars c and c + 1) */
            uint32_t c2 = (c + 1) & 0x3FF, st2 = (cs[c2 >> 2] >> ((c2 & 3) * 2)) & 3;
            if (!st2) {
                const uint32_t *t = (const uint32_t *)(vr + ((chw * 2 + c2 * tb) & 0xFFFF));
                uint32_t o = 0;
                for (uint32_t k = 0; k < tb / 4; k++) o |= t[k];
                st2 = o ? 2 : 1;
                cs[c2 >> 2] |= (uint8_t)(st2 << ((c2 & 3) * 2));
            }
            if (st2 == 2) st = 2;
        }
        if (!st) {
            const uint32_t *t = (const uint32_t *)(vr + ((chw * 2 + c * tb) & 0xFFFF));
            uint32_t o = 0;
            for (uint32_t k = 0; k < tb / 4; k++) o |= t[k];
            st = o ? 2 : 1;
            cs[c >> 2] |= (uint8_t)(st << ((c & 3) * 2));
        }
        any = st == 2;
    }
    rc[ty] = (uint16_t)(stamp | any);
    return any;
}

/* one BG layer's tiles of line y with priority pr into lb (lb[8 + x] = SNES x), src[] = layer id */
static void fe_tiles(uint8_t *lb, uint8_t *src, int li, int bpp, int y, int pr, uint32_t mode, int xa, int xb)
{
    uint32_t sc = game_ppu(0x2107 + li), nba = game_ppu(0x210B + (li >> 1));
    uint32_t mapw = (sc & 0xFC) << 8, msz = sc & 3;
    uint32_t chw = ((li & 1) ? nba >> 4 : nba & 15) << 12;
    uint32_t h = li ? FE_HOFS(li) : (uint32_t)FE_L16[L_H1][y], v = li ? FE_VOFS(li) : (uint32_t)FE_L16[L_V1][y];
    uint32_t my = (uint32_t)(y + 1 + v) & 0x3FF;   /* (line y = SNES scanline y + 1) */
    h &= 0x3FF;
    uint32_t ty = my >> 3, row = my & 7;
    uint32_t mrow = mapw + (ty & 31) * 32;
    if ((msz & 2) && (ty & 32)) mrow += (msz & 1) ? 0x800 : 0x400;
    const uint32_t *X = FE_EXP;
    int32_t x0 = 8 - (int32_t)(h & 7);
    uint32_t tx0 = h >> 3, tb = bpp * 8;
    uint16_t *ment = FE_MENT[li];         /* the line's map entries: read in the priority 0 pass */
    int k0 = xa >> 3, k1 = (xb + (int)(h & 7) + 7) >> 3;   /* (the tiles over [xa, xb)) */
    if (k1 > 33) k1 = 33;
    x0 += 8 * k0;
    const uint8_t *vr = fe_vr;            /* (FE is uncached: read once) */
    uint8_t *cs = fe_chst(li, bpp);       /* (I11) blank characters: skipped before their bytes are read */
    if (!pr) {
        uint32_t vm = fe_vmask;
        const uint16_t *vmw = UNCACHED(FE_VM);
        for (int k = k0; k < k1; k++) {
            uint32_t tx = (tx0 + k) & 63, a = (mrow + (tx & 31) + ((msz & 1) && (tx & 32) ? 0x400 : 0)) & 0x7FFF;
            uint32_t e = a >= 0x7000 && (vm & bit32[(a - 0x7000) >> 8]) ? vmw[a - 0x7000] : FE_VM_UNSET;
            ment[k] = (uint16_t)(e != FE_VM_UNSET ? e : FE_LE16(vr + a * 2));
        }
    }
    for (int k = k0; k < k1; k++, x0 += 8) {
        uint32_t e = ment[k];
        if ((uint32_t)((e >> 13) & 1) != (uint32_t)pr) continue;
        {
            uint32_t c = e & 0x3FF, q = c & 3, cb = cs[c >> 2];
            if (!(cb & fe_cs_known[q])) {
                const uint32_t *tt = (const uint32_t *)(vr + ((chw * 2 + c * tb) & 0xFFFF));
                uint32_t o = 0;
                for (uint32_t i = 0; i < tb / 4; i++) o |= tt[i];
                cb |= o ? (uint8_t)(fe_cs_known[q] ^ fe_cs_blank[q]) : fe_cs_blank[q];
                cs[c >> 2] = (uint8_t)cb;
            }
            if (cb & fe_cs_blank[q]) continue;
        }
        uint32_t r = (e & 0x8000) ? 7 - row : row;
        const uint8_t *t = vr + ((chw * 2 + (e & 0x3FF) * tb + r * 2) & 0xFFFF);
        uint32_t w01 = *(const uint16_t *)t;  /* (one 16-bit read per two bit planes: big-endian = t[0] t[1]) */
        if (bpp == 2 && !w01) continue;
        uint32_t w23 = bpp >= 4 ? *(const uint16_t *)(t + 16) : 0;
        if (bpp == 4 && !(w01 | w23)) continue;
        uint32_t b0 = FE_HI8(w01), b1 = FE_LO8(w01);
        uint32_t ph = X[b0 * 2] | X[b1 * 2] << 1, pl = X[b0 * 2 + 1] | X[b1 * 2 + 1] << 1;
        if (bpp >= 4) {
            uint32_t b2 = FE_HI8(w23), b3 = FE_LO8(w23);
            ph |= X[b2 * 2] << 2 | X[b3 * 2] << 3;
            pl |= X[b2 * 2 + 1] << 2 | X[b3 * 2 + 1] << 3;
            if (bpp == 8) {
                ph |= X[t[32] * 2] << 4 | X[t[33] * 2] << 5 | X[t[48] * 2] << 6 | X[t[49] * 2] << 7;
                pl |= X[t[32] * 2 + 1] << 4 | X[t[33] * 2 + 1] << 5 | X[t[48] * 2 + 1] << 6 | X[t[49] * 2 + 1] << 7;
            }
        }
        if (!(ph | pl)) continue;
        uint32_t pal = bpp == 8 ? 0 : bpp == 4 ? ((e >> 10) & 7) << 4 : ((e >> 10) & 7) << 2;
        if (mode == 0) pal += li * 32;
        if (e & 0x4000) { uint32_t t2 = __builtin_bswap32(pl); pl = __builtin_bswap32(ph); ph = t2; }   /* h flip */
        ph = FE_W(ph); pl = FE_W(pl);         /* (lane 0 first in memory) */
        if (x0 >= 0 && !(x0 & 3) && x0 <= 264) {
            uint32_t *d = (uint32_t *)&lb[x0];
            uint32_t mh = fe_lanemask(ph), ml = fe_lanemask(pl), pw = pal * 0x01010101u;
            d[0] = (d[0] & ~mh) | ((ph + pw) & mh);
            d[1] = (d[1] & ~ml) | ((pl + pw) & ml);
            if (src) {
                uint32_t *sw = (uint32_t *)&src[x0], lw = (uint32_t)li * 0x01010101u;
                sw[0] = (sw[0] & ~mh) | (lw & mh);
                sw[1] = (sw[1] & ~ml) | (lw & ml);
            }
        } else if (x0 >= 0 && x0 <= 260) {
            /* (I12) not on a word: the 8 pixels over 3 words (constant shifts - the SH-2 has no barrel
               shifter; the byte loop below was most of a scrolled BG's time: the Nintendo logo's smoke) */
            uint32_t *d = (uint32_t *)&lb[x0 & ~3];
            uint32_t mh = fe_lanemask(ph), ml = fe_lanemask(pl), pw = pal * 0x01010101u;
            uint32_t vh = (ph + pw) & mh, vl = (pl + pw) & ml, a0, a1, a2, m0, m1, m2;
            switch (x0 & 3) {
            case 1:
                a0 = FE_LATER(vh, 8); a1 = FE_EARLIER(vh, 24) | FE_LATER(vl, 8); a2 = FE_EARLIER(vl, 24);
                m0 = FE_LATER(mh, 8); m1 = FE_EARLIER(mh, 24) | FE_LATER(ml, 8); m2 = FE_EARLIER(ml, 24);
                break;
            case 2:
                a0 = FE_LATER(vh, 16); a1 = FE_EARLIER(vh, 16) | FE_LATER(vl, 16); a2 = FE_EARLIER(vl, 16);
                m0 = FE_LATER(mh, 16); m1 = FE_EARLIER(mh, 16) | FE_LATER(ml, 16); m2 = FE_EARLIER(ml, 16);
                break;
            default:
                a0 = FE_LATER(vh, 24); a1 = FE_EARLIER(vh, 8) | FE_LATER(vl, 24); a2 = FE_EARLIER(vl, 8);
                m0 = FE_LATER(mh, 24); m1 = FE_EARLIER(mh, 8) | FE_LATER(ml, 24); m2 = FE_EARLIER(ml, 8);
                break;
            }
            d[0] = (d[0] & ~m0) | a0;
            d[1] = (d[1] & ~m1) | a1;
            d[2] = (d[2] & ~m2) | a2;
            if (src) {
                uint32_t *sw = (uint32_t *)&src[x0 & ~3], lw = (uint32_t)li * 0x01010101u;
                sw[0] = (sw[0] & ~m0) | (lw & m0);
                sw[1] = (sw[1] & ~m1) | (lw & m1);
                sw[2] = (sw[2] & ~m2) | (lw & m2);
            }
        } else {
            uint8_t px[8] __attribute__((aligned(4)));
            *(uint32_t *)px = ph; *(uint32_t *)(px + 4) = pl;
            for (int i = 0; i < 8; i++) {
                int32_t x = x0 + i;
                if (px[i] && x >= 0 && x < 272) { lb[x] = (uint8_t)(px[i] + pal); if (src) src[x] = (uint8_t)li; }
            }
        }
    }
}

/* a char row -> 8 pixels (lane decode, FE_EXP) */
static inline void fe_row8(const uint8_t *t, int bpp, uint8_t *px)
{
    const uint32_t *X = FE_EXP;
    uint32_t ph = X[t[0] * 2] | X[t[1] * 2] << 1, pl = X[t[0] * 2 + 1] | X[t[1] * 2 + 1] << 1;
    if (bpp >= 4) { ph |= X[t[16] * 2] << 2 | X[t[17] * 2] << 3; pl |= X[t[16] * 2 + 1] << 2 | X[t[17] * 2 + 1] << 3; }
    for (int i = 0; i < 4; i++) { px[i] = (uint8_t)(ph >> (24 - 8 * i)); px[4 + i] = (uint8_t)(pl >> (24 - 8 * i)); }
}

/* hi-res (mode 5): 16-pixel-wide tiles (two chars), every other pixel of the 512 */
static void fe_tiles_hires(uint8_t *lb, uint8_t *src, int li, int bpp, int y, int pr)
{
    uint32_t sc = game_ppu(0x2107 + li), nba = game_ppu(0x210B);
    uint32_t mapw = (sc & 0xFC) << 8, msz = sc & 3;
    uint32_t chw = ((li & 1) ? nba >> 4 : nba & 15) << 12;
    uint32_t h = li ? FE_HOFS(li) : (uint32_t)FE_L16[L_H1][y], v = li ? FE_VOFS(li) : (uint32_t)FE_L16[L_V1][y];
    uint32_t my = (uint32_t)(y + 1 + v) & 0x3FF, ty = my >> 3, row = my & 7;
    uint32_t mrow = mapw + (ty & 31) * 32;
    if ((msz & 2) && (ty & 32)) mrow += (msz & 1) ? 0x800 : 0x400;
    h &= 0x3FF;
    uint32_t phase = h & 1, tx0 = h >> 4, tb = bpp * 8;
    int32_t x0 = 8 - (int32_t)((h & 15) >> 1);
    const uint8_t *vr = fe_vr;
    uint8_t *cs = fe_chst(li, bpp);       /* (I12) blank characters skipped (as fe_tiles) */
    for (int k = 0; k < 34; k++, x0 += 8) {
        uint32_t tx = (tx0 + k) & 63;
        uint32_t e = fe_vword(mrow + (tx & 31) + ((msz & 1) && (tx & 32) ? 0x400 : 0));
        if ((uint32_t)((e >> 13) & 1) != (uint32_t)pr) continue;
        uint32_t r = (e & 0x8000) ? 7 - row : row, t = e & 0x3FF, bl = 0;
        for (int i = 0; i < 2; i++) {     /* (bl bit i: char t + i is blank) */
            uint32_t c = (t + i) & 0x3FF, q = c & 3, cb = cs[c >> 2];
            if (!(cb & fe_cs_known[q])) {
                const uint32_t *tt = (const uint32_t *)(vr + ((chw * 2 + c * tb) & 0xFFFF));
                uint32_t o = 0;
                for (uint32_t j = 0; j < tb / 4; j++) o |= tt[j];
                cb |= o ? (uint8_t)(fe_cs_known[q] ^ fe_cs_blank[q]) : fe_cs_blank[q];
                cs[c >> 2] = (uint8_t)cb;
            }
            if (cb & fe_cs_blank[q]) bl |= 1u << i;
        }
        if (bl == 3) continue;
        uint8_t hp[16] __attribute__((aligned(4)));
        if (bl & 1) { *(uint32_t *)hp = 0; *(uint32_t *)(hp + 4) = 0; }
        else fe_row8(vr + ((chw * 2 + t * tb + r * 2) & 0xFFFF), bpp, hp);
        if (bl & 2) { *(uint32_t *)(hp + 8) = 0; *(uint32_t *)(hp + 12) = 0; }
        else fe_row8(vr + ((chw * 2 + ((t + 1) & 0x3FF) * tb + r * 2) & 0xFFFF), bpp, hp + 8);
        uint32_t pal = ((e >> 10) & 7) << (bpp == 4 ? 4 : 2);
        for (int j = 0; j < 8; j++) {
            uint32_t q = phase + 2 * j, c = (e & 0x4000) ? hp[15 - q] : hp[q];
            int32_t x = x0 + j;
            if (c && x >= 0 && x < 272) { lb[x] = (uint8_t)(c + pal); if (src) src[x] = (uint8_t)li; }
        }
    }
}

/* mode 7 line y (screen over: transparent) */
static void fe_m7(uint8_t *lb, int y)
{
    int32_t a = FE_L16[L_M7A][y], b = FE_L16[L_M7B][y], c = FE_L16[L_M7C][y], d = FE_L16[L_M7D][y];
    int32_t cx = fe_ppu.m7[4], cy = fe_ppu.m7[5];
    int32_t hs = (int16_t)(FE_L16[L_H1][y] << 3) >> 3, vs = (int16_t)(FE_L16[L_V1][y] << 3) >> 3;
    int32_t xx = hs - cx, yy = y + 1 + vs - cy;
    int32_t X = a * xx + b * yy + (cx << 8), Y = c * xx + d * yy + (cy << 8);
    const uint8_t *v = fe_vr;
    /* the x range whose map point is inside the non-blank box (X, Y in 1/256 px, linear in x) */
    int32_t xa = 0, xb = 256;
    {
        int32_t lo[2] = { FE->m7x0 << 8, FE->m7y0 << 8 }, hi[2] = { FE->m7x1 << 8, FE->m7y1 << 8 };
        int32_t p0[2] = { X, Y }, dp[2] = { a, c };
        for (int k = 0; k < 2; k++) {
            if (dp[k] == 0) { if (p0[k] < lo[k] || p0[k] >= hi[k]) return; continue; }
            /* lo <= p0 + dp x < hi */
            int32_t e0, e1;
            if (dp[k] > 0) { e0 = (lo[k] - p0[k] + dp[k] - 1) / dp[k]; e1 = (hi[k] - p0[k] + dp[k] - 1) / dp[k]; }
            else { e0 = (p0[k] - hi[k]) / -dp[k] + 1; e1 = (p0[k] - lo[k]) / -dp[k] + 1; }
            if (lo[k] - p0[k] > 0 && dp[k] > 0 && e0 < 0) e0 = 0;
            if (e0 > xa) xa = e0;
            if (e1 < xb) xb = e1;
        }
        if (xa < 0) xa = 0;
        if (xb > 256) xb = 256;
        if (xa >= xb) return;
        X += a * xa; Y += c * xa;
    }
    if (FE->m7w) {                        /* (the SDRAM copy of the box) */
        const uint8_t *mp = FE_M7MAP, *cp = FE_M7CHR;
        uint32_t w = FE->m7w, bw = FE->m7x1 - FE->m7x0, bh = FE->m7y1 - FE->m7y0;
        X -= FE->m7x0 << 8; Y -= FE->m7y0 << 8;
        if (c == 0) {                     /* (I12) no rotation (the Rare logo's zoom): one map row per line */
            uint32_t py = (uint32_t)(Y >> 8);
            if (py >= bh) return;
            const uint8_t *mr = mp + (py >> 3) * w, *cr = cp + (py & 7) * 8;
            uint8_t *o = lb + 8;
            for (int x = xa; x < xb; x++, X += a) {
                uint32_t px = (uint32_t)(X >> 8);
                if (px >= bw) continue;
                uint32_t col = cr[mr[px >> 3] * 64 + (px & 7)];
                if (col) o[x] = (uint8_t)col;
            }
            return;
        }
        for (int x = xa; x < xb; x++, X += a, Y += c) {
            uint32_t px = (uint32_t)(X >> 8), py = (uint32_t)(Y >> 8);
            if (px >= bw || py >= bh) continue;
            uint32_t col = cp[mp[(py >> 3) * w + (px >> 3)] * 64 + (py & 7) * 8 + (px & 7)];
            if (col) lb[8 + x] = (uint8_t)col;
        }
        return;
    }
    for (int x = xa; x < xb; x++, X += a, Y += c) {
        uint32_t px = (uint32_t)(X >> 8), py = (uint32_t)(Y >> 8);
        if ((px | py) & ~1023u) continue;
        uint32_t t = v[((py >> 3) * 128 + (px >> 3)) * 2];
        uint32_t col = v[(t * 64 + (py & 7) * 8 + (px & 7)) * 2 + 1];
        if (col) lb[8 + x] = (uint8_t)col;
    }
}

/* the BGs of one screen (main or sub: layer mask lay) on line y */
static void fe_layers(uint8_t *lb, uint8_t *src, uint32_t lay, int y, uint32_t m, int xa, int xb)
{
    uint32_t mode = m & 7;
    if (mode == 7) { if (lay & 1) fe_m7(lb, y); return; }
    static const uint8_t bpps[8][4] = { { 2, 2, 2, 2 }, { 4, 4, 2, 0 }, { 4, 4, 0, 0 }, { 8, 4, 0, 0 },
                                        { 8, 2, 0, 0 }, { 4, 2, 0, 0 }, { 4, 0, 0, 0 }, { 0, 0, 0, 0 } };
    /* back to front (layer, priority) */
    static const int8_t ord1[] = { 2, 0, 2, 1, 1, 0, 0, 0, 1, 1, 0, 1, -1 };
    static const int8_t ord1h[] = { 2, 0, 1, 0, 0, 0, 1, 1, 0, 1, 2, 1, -1 };
    static const int8_t ord0[] = { 3, 0, 2, 0, 3, 1, 2, 1, 1, 0, 0, 0, 1, 1, 0, 1, -1 };
    const int8_t *o = mode == 0 ? ord0 : (mode == 1 && (m & 8)) ? ord1h : ord1;
    for (; o[0] >= 0; o += 2) {
        int li = o[0], bpp = bpps[mode][li];
        if (!(lay >> li & 1) || !bpp) continue;
        if (mode == 5) fe_tiles_hires(lb, src, li, bpp, y, o[1]);
        else fe_tiles(lb, src, li, bpp, y, o[1], mode, xa, xb);
    }
}

/* a colour sum's CRAM index (128 + k), or the main colour if the table is full */
static uint32_t fe_pair(uint32_t m, uint32_t s)
{
    uint32_t key = m | s << 8, h = (key * 0x9E37u >> 7) & 511;
    uint8_t *ht = FE_PHASH;
    for (;;) {
        uint32_t k = ht[h];
        if (!k) break;
        if (FE_PKEY[k - 1] == key) return 128 + k;
        h = (h + 1) & 511;
    }
    if (fe_npair >= 126) return 0;        /* (full: the caller mixes the two) */
    FE_PKEY[fe_npair++] = (uint16_t)key;
    ht[h] = (uint8_t)fe_npair;
    return 128 + fe_npair;
}

static void fe_line(int y, volatile uint8_t *dst, int obj)
{
#if defined(FEDBG) && !defined(FEHOST)
    uint16_t dbg_t0 = frt_read();
#endif
    int xa = FE->xa, xb = FE->xb;
    uint8_t *lb = FE_LB, *sb = FE_SB, *src = FE_SRC;    /* (not on the stack: 1.5 KB) */
    uint32_t m = FE_L8[L_MODE][y], tm = FE_L8[L_TM][y], ts = FE_L8[L_TS][y];
    uint32_t cgw = FE_L8[L_CGW][y], cga = FE_L8[L_CGA][y];
    if (FE->subm) { tm = ts; ts = 0; cga = 0; }      /* (the backdrop is added by the palette) */
    if (FE->gplane) { tm &= ~2u; ts &= ~2u; }        /* (I11: BG2 is the Genesis plane) */
    int math = (cga & 0x3F) && !(cga & 0x80) && (cgw & 2) && (ts & 0x0F);   /* add the subscreen's BGs */
    for (int i = 0; i < 272; i += 4) *(uint32_t *)&lb[i] = 0;
    if (math) for (int i = 0; i < 272; i += 4) *(uint32_t *)&src[i] = 0x05050505u;
    fe_layers(lb, math ? src : 0, tm & 0x0F, y, m, xa, xb);
    uint32_t used[4] = { 0, 0, 0, 0 }, ovf[4] = { 0, 0, 0, 0 }, du[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    if (math) {
        uint32_t sdark[8], mdark[4], lk = 0xFFFFFFFFu, lr = 0x100, *prc = FE_PRC;
        const uint8_t *brt = FE_BRT;
        for (int i = 0; i < 8; i++) sdark[i] = FE->sdark[i];
        for (int i = 0; i < 4; i++) mdark[i] = FE->mdark[i];
        for (int i = 0; i < 272; i += 4) *(uint32_t *)&sb[i] = 0;
        fe_layers(sb, 0, ts & 0x0F, y, m, xa, xb);
        for (int x = 8 + xa; x < 8 + xb; x++) {
            uint32_t s = sb[x];
            if (!s || !(cga & bit32[src[x]])) continue;
            uint32_t mm = lb[x];
            if (obj) {                    /* (main transparent: the sub, whatever the backdrop - a backdrop
                                             flash would otherwise redraw every line: H8) */
                if (!mm) lb[x] = (uint8_t)s;
                else {
                    /* (no colour sums with sprites shown - their palettes fill CRAM 128-255): one of the
                       two where the other is dark now, else (I5) a checkerboard of both - the intro's
                       platform -> jungle cross-fade fades evenly with the palettes instead of switching
                       colour by colour */
                    used[mm >> 5] |= bit32[mm & 31];
                    int md = (mdark[mm >> 5] & bit32[mm & 31]) != 0, sd = 0;
                    if (s < 128) { used[s >> 5] |= bit32[s & 31]; sd = (mdark[s >> 5] & bit32[s & 31]) != 0; }
                    if (md && !sd) lb[x] = (uint8_t)s;
                    else if (!md && !sd && ((x + y) & 1)) lb[x] = (uint8_t)s;
                }
            }
            else {
                /* (I12) the last pair's result again (runs of the same two colours: the sum's lookup and the
                   "brighter of the two" were done per pixel) - lr 0x100: the main colour stays */
                uint32_t key = mm | s << 8;
                if (key != lk) {
                    lk = key;
                    uint32_t *ce = prc + ((key * 0x9E37u >> 8) & 127), e = *ce, fl;
                    if ((e & 0xFFFF) == key) { lr = (e >> 16) & 0x1FF; fl = e >> 25; }
                    else {
                        fl = 0;
                        if (sdark[s >> 5] & bit32[s & 31]) lr = 0x100;    /* (adds next to nothing: no sum) */
                        else if (fl = 1, sdark[mm >> 5] & bit32[mm & 31]) lr = s;   /* (dark + s ~ s) */
                        else {
                            uint32_t pc = fe_pair(mm, s);
                            if (pc) lr = pc;
                            else {
                                /* more sums than CRAM entries (the Rare logo: 29 grid x 224 logo colours): the
                                   brighter of the two - (I5) recorded, so the line is drawn again when a colour's
                                   brightness changes (fe_ovf_dirty: the grid stayed on the finished logo) */
                                fl = 3;
                                lr = brt[mm & 255] < brt[s & 255] ? s : 0x100;
                            }
                        }
                        *ce = key | lr << 16 | fl << 25;
                    }
                    du[s >> 5] |= bit32[s & 31];
                    if (fl & 1) du[mm >> 5] |= bit32[mm & 31];
                    if (fl & 2) { ovf[(mm & 127) >> 5] |= bit32[mm & 31]; ovf[(s & 127) >> 5] |= bit32[s & 31]; }
                }
                if (lr != 0x100) lb[x] = (uint8_t)lr;
            }
        }
    }
    if (!FE->m7w && xa == 0 && xb == 256) {   /* (whole lines only: the records must be complete) */
        uint32_t *o = FE_OVF[y];
        o[0] = ovf[0]; o[1] = ovf[1]; o[2] = ovf[2]; o[3] = ovf[3];
    }
    {
        uint32_t *u = FE_DUSED[y];
        if (xa == 0 && xb == 256) for (int k = 0; k < 8; k++) u[k] = du[k];
        else for (int k = 0; k < 8; k++) u[k] |= du[k];
    }
    if (obj && !FE->m7w && xa == 0 && xb == 256) {
        uint32_t *u = FE_LUSED[y];
        u[0] = used[0]; u[1] = used[1]; u[2] = used[2]; u[3] = used[3];
    }
    volatile uint32_t *d = (volatile uint32_t *)dst;
    for (int i = xa >> 2; i < xb >> 2; i++) d[i] = *(uint32_t *)&lb[8 + i * 4];
#if defined(FEDBG) && !defined(FEHOST)
    fedbg_tline += (uint16_t)(frt_read() - dbg_t0);
#endif
#ifdef FEDBG2
    {   /* (test: right border bits = non-zero main pixels before math / sub pixels, 8 bits each) */
        uint32_t nm = 0, ns = 0;
        for (int x = 8; x < 264; x++) { nm += sb[x] != 0; }
        for (int x = 8; x < 264; x++) { ns += lb[x] != 0; }
        volatile uint8_t *r = dst + 256;
        for (int b = 0; b < 16; b++) r[b] = ((b < 8 ? nm >> (7 - b) : ns >> (15 - b)) & 1) ? 255 : FE_BORDER;
        r[16] = math ? 255 : FE_BORDER;
    }
#endif
}

static void fe_lines(int y0, int y1)
{
    int obj = fe_obj;
    for (int y = y0; y < y1; y++) fe_line(y, FB8 + FB_BASE + y * CV_P + CV_XOFF + 32, obj);
}

/* what the picture depends on (besides the palette) */
static uint32_t fe_signature(void)
{
    /* (I11: map writes are in the line signatures per map row now - a text box typed, or a map uploaded
       again unchanged every frame, redrew every line; mode 7 screens: as before) */
    uint32_t s = (FE->m7 || FE->m7w) ? fe_vgen * 0x9E3779B1u : 0x811C9DC5u;
    static const uint16_t r[] = { 0x2107, 0x2108, 0x2109, 0x210A, 0x210B, 0x210C, 0x211A };
    for (uint32_t i = 0; i < sizeof r / sizeof r[0]; i++) s = (s ^ game_ppu(r[i])) * 0x01000193u;
    const uint16_t *p = (const uint16_t *)&fe_ppu, *q = FE_SCRL[0];
    uint32_t lay = 0;                     /* the BGs shown on some line (main, or sub with colour math) */
    for (int y = 0; y < SCREEN_H; y++) lay |= FE_L8[L_TM][y] | FE_L8[L_TS][y];
    if (FE->gplane) lay &= ~2u;           /* (I11: BG2 on the Genesis) */
    for (uint32_t i = FE->perline ? 3 : 0; i < 4; i++)   /* (perline: BG1 - BG3 in the line signatures) */
        if (lay & bit32[i]) s = ((s ^ q[i]) * 0x01000193u ^ q[4 + i]) * 0x01000193u;   /* its scroll */
    if (FE->m7) for (uint32_t i = 8; i < 16; i++) s = (s ^ p[i]) * 0x01000193u;   /* mode 7 */
    if (fe_obj && FE->m7w)                /* dark-main rule: which BG colours are dark decides (else per line) */
        for (int i = 0; i < 4; i++) s = (s ^ FE->mdark[i]) * 0x01000193u;
    s = (s ^ (uint32_t)FE->subm) * 0x01000193u;
    return s ^ (uint32_t)fe_idx;
}

static uint16_t fe_scale(uint32_t c, uint32_t br)
{
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return (uint16_t)(0x8000 | (r * br >> 4) | (g * br >> 4) << 5 | (b * br >> 4) << 10);
}
static uint16_t fe_add(uint32_t m, uint32_t s)
{
    uint32_t r = (m & 31) + (s & 31), g = ((m >> 5) & 31) + ((s >> 5) & 31), b = ((m >> 10) & 31) + ((s >> 10) & 31);
    if (fe_half) { r >>= 1; g >>= 1; b >>= 1; }
    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (b > 31) b = 31;
    return (uint16_t)(r | g << 5 | b << 10);
}

/* (vblank, after the flip) the game's palette at its brightness */
static void fe_palette(void)
{
    uint32_t br = game_dark() ? 0 : (game_inidisp & 15) + 1;
    uint32_t sum = br * 0x10001u + fe_npair * 77 + FE->subm * 0x55555 + FE->gplane * 0x3333;
    for (int i = 0; i < 128; i++) sum = sum * 31 + spr_bgpal[i] + spr_pal[i] * 7;
    if (FE->subm) sum = sum * 31 + fe_ppu.coldata[0] + (fe_ppu.coldata[1] << 5) + (fe_ppu.coldata[2] << 10) + fe_half * 77777;
    if (sum == fe_palsum) return;
    fe_palsum = sum;
    if (FE->subm) {                       /* every colour + the backdrop (the main screen) */
        uint32_t bd = spr_bgpal[0] & 0x7FFF;
        uint32_t fx = fe_ppu.coldata[0] | fe_ppu.coldata[1] << 5 | fe_ppu.coldata[2] << 10;
        for (int i = 1; i < 128; i++) CRAM[i] = fe_scale(fe_add(bd, spr_bgpal[i]), br);
        for (int i = 129; i < 256; i++) CRAM[i] = fe_scale(fe_add(bd, spr_pal[i - 128]), br);
        uint16_t c0 = fe_scale(fe_add(bd, fx), br);   /* (subscreen transparent: the fixed colour) */
        CRAM[0] = c0;
        CRAM[FE_BORDER] = 0x8000;
        for (int i = 0; i < SCREEN_H; i++) *UNCACHED(&sky_ram[i]) = c0;
        return;
    }
    for (int i = 1; i < 128; i++) CRAM[i] = fe_scale(spr_bgpal[i], br);
    if (fe_obj) for (int i = 129; i < 256; i++) CRAM[i] = fe_scale(spr_pal[i - 128], br);
    else for (uint32_t k = 0; k < fe_npair; k++) {
        uint32_t key = FE_PKEY[k], m = key & 255, s = key >> 8;
        CRAM[129 + k] = fe_scale(fe_add(m < 128 ? spr_bgpal[m] : spr_pal[m - 128], s < 128 ? spr_bgpal[s] : spr_pal[s - 128]), br);
    }
    uint16_t bd = fe_scale(spr_bgpal[0], br);
    if (FE->gplane) {
        /* (I11) BG2 on the Genesis: pixels without a 32X layer are see-through (no priority bit - the
           Genesis backdrop is the SNES one); the plane's colours at this brightness (68000 weather keys
           0 = black, 1 = the palette: k / 16) */
        bd &= 0x7FFF;
        COMM(10) = (uint16_t)(0x4000 | 1 << 8 | br);
    }
    CRAM[0] = bd;
    CRAM[FE_BORDER] = 0x8000;
    for (int i = 0; i < SCREEN_H; i++) *UNCACHED(&sky_ram[i]) = bd;
}

/* per-line signatures of what each frame buffer shows (map_mem's lower half: the sprites' chunky tile
   copy is detached on these screens) */
#define FE_LSIG ((uint32_t (*)[SCREEN_H])(uint8_t *)map_mem)
/* (I11, screens without a mode 7 copy - FE_M7CHR uses this memory) per buffer and line the signature
   without the H scrolls (16 bits) of what was drawn; the BG H scroll of the last picture */
#define FE_LSIG2  ((uint16_t (*)[SCREEN_H])((uint8_t *)map_mem + 13184))     /* [2][224] */
#define FE_HPREV  ((uint16_t *)((uint8_t *)map_mem + 14080))                  /* [4] */
/* (I11) per buffer: the frame key (everything the line signatures depend on) it was last drawn
   completely with, and whether that drawing was complete (map_mem 13184-14848 is free here) */
#define FE_FKEY   ((uint32_t *)((uint8_t *)map_mem + 14096))                  /* [2] */
#define FE_FOK    ((uint32_t *)((uint8_t *)map_mem + 14104))                  /* [2] */
/* (I11) this picture's line signatures (fe_lsig) and their "without H scroll" halves (map_mem's upper
   half: no front-end data there) */
#define FE_LSC    ((uint32_t *)((uint8_t *)map_mem + 16384))                  /* [224] */
#define FE_LSC2   ((uint16_t *)((uint8_t *)map_mem + 17280))                  /* [224]: (uint16_t) ns | 1 */
/* (I11) the signatures without the map rows' generations: this picture's, and per buffer what it was drawn
   with (| 1; 0 = none); per buffer and map row of the VRAM mirror (32 words) the columns written
   since its last finished picture (sprites.c fe_dirty) */
#define FE_LSCA   ((uint32_t *)((uint8_t *)map_mem + 17728))                  /* [224] */
#define FE_LSIGA  ((uint32_t (*)[SCREEN_H])((uint8_t *)map_mem + 18624))     /* [2][224] (-> 20416; Torchlight's
                                                                                 beam tables: 20480 - 22272) */
/* (I12) line y of buffer bi just drawn: its stored signatures with the darkness of the colours it used
   now (FE_DUSED as this drawing left it) instead of the ones before (FE_DHC, in FE_LSC & co.) */
static void fe_dh_fix(int bi, int y, const uint32_t *sd, int more)
{
    if (fe_npair >= 126 && FE->m7w) return;   /* (fe_lsig_all's glob) */
    uint32_t d = FE_DHC[y] ^ fe_dh(y, sd);
    if (!d) return;
    FE_LSIG[bi][y] ^= d;
    if (more) { FE_LSIG2[bi][y] ^= (uint16_t)d; FE_LSIGA[bi][y] ^= d; }
}

/* line y's signature: what its picture is drawn from (without the sprites' colour-math records: fe_lsig_all);
   *pns (I11): the same without the BG H scrolls (a line that only scrolled sideways a little), *pa (I11):
   without the map rows' write generations (a text typed in: only the columns written are drawn again) */
static uint32_t fe_lsig_base(int y, uint32_t sig, uint32_t dsig, int perline, int m7, uint32_t *pns, uint32_t *pa)
{
    uint32_t gm = FE->gplane ? ~2u : ~0u; /* (I11: BG2 on the Genesis: not in the lines) */
    uint32_t cga = FE_L8[L_CGA][y];
    uint32_t ls = sig;                    /* (I12: colour math's dark colours per line - fe_lsig_all, FE_DUSED) */
    (void)dsig; (void)cga;
    for (int k = 0; k < NL8; k++) ls = (ls ^ FE_L8[k][y]) * 0x01000193u;
    for (int k = m7 ? 0 : L_H1 + 1; k < NL16; k++) if (k != L_H1) ls = (ls ^ (uint16_t)FE_L16[k][y]) * 0x01000193u;
    uint32_t ns = ls;
    ls = (ls ^ (uint16_t)FE_L16[L_H1][y]) * 0x01000193u;
    if (perline) {                        /* BG2 / BG3: their scroll only if their map row here is not empty */
        uint32_t lay = (FE_L8[L_TM][y] | FE_L8[L_TS][y]) & gm, md = FE_L8[L_MODE][y] & 7;
        for (int li = 1; li < 3; li++) {
            int bpp = md == 5 ? (li == 1 ? 0x82 : 0) : li == 1 ? 4 : md == 1 ? 2 : 0;   /* (0x80: hi-res tiles) */
            if (!(lay & bit32[li]) || !bpp) continue;
            uint32_t vo = FE_VOFS(li), ho = FE_HOFS(li);
            uint32_t f = fe_rowflag(li, ((y + 1 + vo) & 0x3FF) >> 3, bpp);
            ls = (ls ^ (f ? (ho << 16 | (vo & 0xFFFF)) : 0xFFFFFFFFu)) * 0x01000193u;
            ns = (ns ^ (f ? (vo & 0xFFFF) : 0xFFFFFFFFu)) * 0x01000193u;
        }
    }
    *pa = ls;
    uint32_t t = 0x811C9DC5u;             /* (the rest: the same in both) */
    if (!m7 && !FE->m7w) {                /* (I11) the map rows (in the VRAM mirror) its BGs show here: changed */
        uint32_t lay = (FE_L8[L_TM][y] | FE_L8[L_TS][y]) & gm, md = FE_L8[L_MODE][y] & 7;
        for (int li = 0; li < 3; li++) {
            if (!(lay & bit32[li]) || (md == 2 && li == 2) || (md >= 3 && li == 2)) continue;
            uint32_t sc = game_ppu(0x2107 + li), mapw = (sc & 0xFC) << 8, msz = sc & 3;
            uint32_t v = li ? FE_VOFS(li) : (uint32_t)FE_L16[L_V1][y];
            uint32_t ty = ((y + 1 + v) & 0x3FF) >> 3;
            uint32_t mrow = (mapw + (ty & 31) * 32 + ((msz & 2) && (ty & 32) ? ((msz & 1) ? 0x800 : 0x400) : 0)) & 0x7FFF;
            if (mrow >= 0x7000) t = (t ^ FE_TRK->rgen[(mrow - 0x7000) >> 6]) * 0x01000193u;
            if ((msz & 1) && ((mrow + 0x400) & 0x7FFF) >= 0x7000) t = (t ^ FE_TRK->rgen[(((mrow + 0x400) & 0x7FFF) - 0x7000) >> 6] << 8) * 0x01000193u;
        }
    }
    *pns = (ns ^ t) * 0x01000193u;
    return (ls ^ t) * 0x01000193u;
}
/* (I11) every line's signatures into FE_LSC / FE_LSC2 / FE_LSCA, once a picture (they were evaluated up to
   five times a line: a fifth of the cabins' time in ares); a line with the registers and BG map rows of the
   line above (no layer starts a new tile row) has its signatures too, but for its colour-math records */
static uint32_t og_rot;                  /* (I12) pictures: which quarter of the lines checks its ovf colours */
static void fe_lsig_all(uint32_t sig, uint32_t dsig, int perline, int m7, const uint32_t *blk)
{
    uint32_t *lsc = FE_LSC;
    uint16_t *lsc2 = FE_LSC2;
    uint32_t *lsca = FE_LSCA;
    uint32_t b = 0, bn = 0, ba = 0;
    uint32_t vo1 = FE_VOFS(1), vo2 = FE_VOFS(2);
    int k0 = m7 ? 0 : L_H1;
    uint32_t hw[SCREEN_H / 32];           /* lines with an HDMA write (fe_lines_regs): the others have the
                                             registers of the line above */
    for (int w = 0; w < SCREEN_H / 32; w++) {
        uint32_t m = 0;
        for (int k = 0; k < NL8; k++) m |= FE_SET[k][w];
        for (int k = k0; k < NL16; k++) m |= FE_SET[NL8 + k][w];
        hw[w] = m;
    }
    uint32_t sd[8];                       /* (FE is uncached) */
    for (int k = 0; k < 8; k++) sd[k] = FE->sdark[k];
    int glob = fe_npair >= 126 && FE->m7w;   /* (the colour sums used up - fe_pair - on a mode 7 screen) */
    for (int y = 0; y < SCREEN_H; y++) {
        int same = y > 0;
        if (same && (hw[y >> 5] & bit32[y & 31])) {
            for (int k = 0; k < NL8 && same; k++) same = FE_L8[k][y] == FE_L8[k][y - 1];
            for (int k = k0; k < NL16 && same; k++) same = FE_L16[k][y] == FE_L16[k][y - 1];
        }
        if (same && (!((y + 1 + (uint32_t)FE_L16[L_V1][y]) & 7) || !((y + 1 + vo1) & 7) || !((y + 1 + vo2) & 7))) same = 0;
        if (!same) b = fe_lsig_base(y, sig, dsig, perline, m7, &bn, &ba);
        uint32_t ls = b, ns = bn, la = ba;
        if (!fe_obj) {                    /* (I12) colour math: the darkness of the colours it looked at (xor:
                                             fe_dh_fix puts the drawn line's own in once it is drawn) - all of
                                             them while the colour sums are used up (glob: the "brighter of
                                             the two" pixels are not tracked on mode 7 screens, FE_OVF) */
            uint32_t w = glob ? (dsig & ~1u) : fe_dh(y, sd);
            FE_DHC[y] = w;
            ls ^= w; ns ^= w; la ^= w;
        }
        if (blk) {                        /* (sprites + colour math: the black-ness of the main colours this
                                             line's math looked at - a cross-fade redraws only their lines) */
            const uint32_t *u = FE_LUSED[y];
            uint32_t w0 = blk[0] & u[0], w1 = blk[1] & u[1], w2 = blk[2] & u[2], w3 = blk[3] & u[3];
            if (w0 | w1 | w2 | w3) {
                uint32_t w = ((w0 * 0x9E3779B1u ^ w1) * 0x9E3779B1u ^ w2) * 0x9E3779B1u ^ w3;
                ls = (ls ^ w) * 0x01000193u; ns = (ns ^ w) * 0x01000193u; la = (la ^ w) * 0x01000193u;
            }
        }
        lsc[y] = ls;
        lsc2[y] = (uint16_t)ns | 1;
        lsca[y] = la | 1;
    }
}

/* (I11) the x range [*pxa, *pxb) (multiples of 8) of line y over the map cells written since buffer bi last
   finished a picture (FE_DIRTY: the VRAM mirror's writes, per map row of 32 words); 0 = none known (draw
   the whole line).  Mode 1 lines only. */
static int fe_dirty_range(int y, int bi, int32_t *pxa, int32_t *pxb)
{
    const uint32_t *dm = FE_DIRTY + bi * 128;
    uint32_t lay = (FE_L8[L_TM][y] | FE_L8[L_TS][y]) & (FE->gplane ? ~2u : ~0u);
    if ((FE_L8[L_MODE][y] & 7) != 1) return 0;
    int32_t xa = 1 << 20, xb = -(1 << 20);
    for (int li = 0; li < 3; li++) {
        if (!(lay & bit32[li])) continue;
        uint32_t sc = game_ppu(0x2107 + li), mapw = (sc & 0xFC) << 8, msz = sc & 3;
        uint32_t v = li ? FE_VOFS(li) : (uint32_t)FE_L16[L_V1][y];
        uint32_t h = (li ? FE_HOFS(li) : (uint32_t)FE_L16[L_H1][y]) & 0x3FF;
        uint32_t ty = ((y + 1 + v) & 0x3FF) >> 3;
        uint32_t mrow = (mapw + (ty & 31) * 32 + ((msz & 2) && (ty & 32) ? ((msz & 1) ? 0x800 : 0x400) : 0)) & 0x7FFF;
        uint32_t mrow2 = (mrow + 0x400) & 0x7FFF;
        uint32_t m0 = mrow >= 0x7000 ? dm[(mrow - 0x7000) >> 5] : 0;
        uint32_t m1 = !(msz & 1) ? m0 : mrow2 >= 0x7000 ? dm[(mrow2 - 0x7000) >> 5] : 0;
        if (!(m0 | m1)) continue;
        uint32_t tx0 = h >> 3;
        int32_t xo = -(int32_t)(h & 7);
        for (int k = 0; k < 33; k++) {
            uint32_t tx = (tx0 + k) & 63, m = (tx & 32) ? m1 : m0;
            if (m & bit32[tx & 31]) {
                int32_t x = 8 * k + xo;
                if (x < xa) xa = x;
                if (x + 8 > xb) xb = x + 8;
            }
        }
    }
    if (xa < 0) xa = 0;
    xa &= ~7;
    xb = (xb + 7) & ~7;
    if (xb > 256) xb = 256;
    if (xa >= xb) return 0;
    *pxa = xa; *pxb = xb;
    return 1;
}

/* (I11) line y's 8-pixel columns in mask m (bit k: SNES x 8k) from its clean copy: the restore under the
   last sprites (frame buffer reads are slow: not the gaps between them) */
static void fe_restore(int y, uint32_t m)
{
    volatile uint32_t *c = FE_CLEAN(y), *d = (volatile uint32_t *)(FB8 + FB_BASE + y * CV_P + CV_XOFF + 32);
    for (; m; m >>= 1, c += 2, d += 2)
        if (m & 1) { d[0] = c[0]; d[1] = c[1]; }
}

/* (I11) a whole line and its clean copy: a line under sprites without a clean copy (drawn before the
   sprites came - the cabin's fade-in) was redrawn from the cartridge under the sprites at every picture
   (~190 lines a picture in ares: 16 vblanks) - once in full, the restores are copies from then on */
static void fe_line_clean(int y, int bi, int obj)
{
    FE->xa = 0; FE->xb = 256;
    fe_line(y, FB8 + FB_BASE + y * CV_P + CV_XOFF + 32, obj);
    FE->nlines++;
    if (obj && y >= FE_CY0 && y < FE_CY0 + FE_CN) {
        volatile uint32_t *c = FE_CLEAN(y);
        const uint32_t *l = (const uint32_t *)(FE_LB + 8);
        for (int i = 0; i < 64; i++) c[i] = l[i];
        FE->cval[bi][y >> 5] |= bit32[y & 31];
    }
}

static void fe_frame(uint16_t *frame_no)
{
    FEDBG_MARK(0);
#ifdef FEPHASE
    {   extern uint16_t ares_phase; *(volatile uint16_t *)&ares_phase = fe_idx == FEPHASE; }   /* (test: ares profile of one screen) */
#endif
    int bi = cur_fb ^ 1;
    int obj = 0;
    {   /* (I5: the intro's flash to white - TM 0, the scene on the subscreen added to a brightening
           backdrop): every line main = backdrop, math adds the subscreen to it */
        int sm = 1;
        for (int y = 0; y < SCREEN_H && sm; y++) {
            uint32_t cga = FE_L8[L_CGA][y];
            sm = !(FE_L8[L_TM][y] & 0x1F) && (cga & 0x20) && !(cga & 0x80) && (FE_L8[L_CGW][y] & 2) && (FE_L8[L_TS][y] & 0x1F) &&
                 (FE_L8[L_MODE][y] & 7) != 7;
        }
        FE->subm = sm;
    }
    for (int y = 0; y < SCREEN_H; y++) obj |= FE->subm ? FE_L8[L_TS][y] : FE_L8[L_TM][y];
    fe_obj = obj = (obj & 0x10) != 0;
    fe_half = (game_ppu(0x2131) & 0x40) != 0;
    if (FE->pfull && game_stats.frames - FE->pfull_f >= 60) {   /* the colour sums ran out: a new table, all lines */
        FE->pfull = 0;
        FE->pfull_f = game_stats.frames;
        fe_npair = 0;
        for (int i = 0; i < 512; i++) FE_PHASH[i] = 0;
        fe_palsum = 0xFFFFFFFF;
        for (int y = 0; y < SCREEN_H; y++) FE_LSIG[0][y] = FE_LSIG[1][y] = 0xFFFFFFFF;
        if (!FE->m7w) for (int y = 0; y < SCREEN_H; y++) FE_LSIG2[0][y] = FE_LSIG2[1][y] = 0;   /* (0: none) */
        for (int y = 0; y < SCREEN_H; y++) FE_LSIGA[0][y] = FE_LSIGA[1][y] = 0;
    }
    for (int i = 0; i < 8; i++) {         /* (palette fades: per frame) */
        uint32_t m = 0;
        for (int k = 0; k < 32; k++) {
            uint32_t c = (i < 4 ? spr_bgpal[i * 32 + k] : spr_pal[(i - 4) * 32 + k]) & 0x7FFF;
            if ((c & 31) + ((c >> 5) & 31) + ((c >> 10) & 31) < 8) m |= bit32[k];
        }
        FE->sdark[i] &= m;                /* (sticky: a colour seen lit stays "lit" - palette cycling /
                                             fades no longer redraw lines over and over: H4, H10) */
    }
    uint32_t sdk[8];                      /* (a cached copy: fe_dh_fix) */
    for (int i = 0; i < 8; i++) sdk[i] = FE->sdark[i];
    {   /* (I12) fe_line's colour math: this picture's pair results (none yet), the colours' brightness */
        uint32_t *prc = FE_PRC;
        uint8_t *brt = FE_BRT;
        for (int i = 0; i < 128; i++) prc[i] = 0;
        for (int i = 0; i < 256; i++) {
            uint32_t c = i < 128 ? spr_bgpal[i] : spr_pal[i - 128];
            brt[i] = (uint8_t)((c & 31) + ((c >> 5) & 31) + ((c >> 10) & 31));
        }
    }
    {   /* modes 1 / 3 / 5 only: BG2 / BG3 scroll per line (fe_rowflag) - (I5) the Nintendo logo's lines are
           modes 1 and 5: its shine (BG3, moved to a random place every 9 frames) redrew every line, the
           time budget split that into slices of old and new = streaks */
        int pl = 1;
        for (int y = 0; y < SCREEN_H && pl; y++) { uint32_t md = FE_L8[L_MODE][y] & 7; pl = md == 1 || md == 3 || md == 5; }
        FE->perline = pl;
    }
    int perline = FE->perline;
    uint32_t blkm[4], *blk = 0;
    for (int i = 0; i < 4; i++) {         /* (the BG colours that are dark now) */
        uint32_t m = 0;
        for (int k = 0; k < 32; k++) {
            uint32_t c = spr_bgpal[i * 32 + k] & 0x7FFF;
            if ((c & 31) + ((c >> 5) & 31) + ((c >> 10) & 31) < 8) m |= bit32[k];
        }
        blkm[i] = FE->mdark[i] = m;
    }
    if (obj && !FE->m7w) blk = blkm;
    uint32_t og = 0;
    int lchg = 0;
    if (!FE->m7w) {                       /* (I5) the colours' brightness levels: changes -> FE_LGEN */
        if (!FE->ogen) {
            for (int y = 0; y < SCREEN_H; y++) { uint32_t *o = FE_OVF[y]; o[0] = o[1] = o[2] = o[3] = 0; FE_DGEN[0][y] = FE_DGEN[1][y] = 0; }
            for (int i = 0; i < 128; i++) FE_LGEN[i] = 0;
            for (int i = 0; i < 256; i++) FE_LVL[i] = 0xFF;
            FE->ogen = 1;
        }
        og = ++FE->ogen & 0xFFFF;
        if (!og) og = FE->ogen = 1;
        for (int i = 0; i < 256; i++) {
            uint32_t c = i < 128 ? spr_bgpal[i] : spr_pal[i - 128];
            uint32_t l = ((c & 31) + ((c >> 5) & 31) + ((c >> 10) & 31)) >> 2;
            if (l != FE_LVL[i]) { FE_LVL[i] = (uint8_t)l; FE_LGEN[i & 127] = (uint16_t)og; lchg = 1; }
        }
    }
    if (FE->gplane) {                     /* (I11) the plane's clouds still the game's? (a map written since) */
        uint32_t sc = game_ppu(0x2108), base = (sc & 0xFC) << 8, h = sc;
        if (base >= 0x7000)
            for (uint32_t r = (base - 0x7000) >> 6; r < 64 && r < ((base - 0x7000) >> 6) + 32; r++) h = h * 31 + FE_TRK->rgen[r];
        if (h != FE->grg) {
            FE->grg = h;
            if (fe_bg2_sum(0) != FE->gsum) {
                /* no: BG2 drawn here again, every line; the Genesis planes cleared */
                FE->gplane = 0;
                COMM(C_CMD) = 0x8000 | 0x7FFF;
                while (COMM(C_CMD) & 0x8000)
                    ;
                for (int y = 0; y < SCREEN_H; y++) {
                    FE_LSIG[0][y] = FE_LSIG[1][y] = 0xFFFFFFFF;
                    FE_LSIG2[0][y] = FE_LSIG2[1][y] = 0;
                    FE_LSIGA[0][y] = FE_LSIGA[1][y] = 0;
                }
                FE_FOK[0] = FE_FOK[1] = 0;
                fe_palsum = 0xFFFFFFFF;
            }
        }
    }
    uint32_t sig = fe_signature();
    uint32_t dsig = sig;                  /* (lines with colour math: the dark colours decide too) */
    for (int i = 0; i < 8; i++) dsig = (dsig ^ FE->sdark[i]) * 0x01000193u;
    const uint32_t (*mk)[2] = (const uint32_t (*)[2])UNCACHED(&spr_marks[bi][0]);
    int16_t mxa[SCREEN_H / 8], mxb[SCREEN_H / 8];   /* (I11) per canvas tile row: the SNES x range under the last
                                                      sprites (canvas column c -> SNES x 8 (c - 4)), empty: none */
    uint32_t mcol[SCREEN_H / 8];          /* (I11) ... and its 8-pixel columns (bit k: SNES x 8k): restored one by one */
    for (int r = 0; r < SCREEN_H / 8; r++) {
        uint32_t m0 = mk[r][0], m1 = mk[r][1];
        mxa[r] = mxb[r] = 0;
        mcol[r] = m0 >> 4 | m1 << 28;
        if (!obj || !(m0 | m1)) continue;
        int c0 = 0, c1 = 63;
        while (!((c0 & 32 ? m1 : m0) & bit32[c0 & 31])) c0++;
        while (!((c1 & 32 ? m1 : m0) & bit32[c1 & 31])) c1--;
        int32_t xa = (c0 - 4) * 8, xb = (c1 - 3) * 8;
        if (xa < 0) xa = 0;
        if (xb > 256) xb = 256;
        if (xa < xb) { mxa[r] = (int16_t)xa; mxb[r] = (int16_t)xb; }
    }
    int m7 = FE->m7;
    FE->nlines = 0;
    /* (I11) the frame key: everything the line signatures depend on.  The same as when this buffer was
       last drawn completely -> every line is as it is: only the restores under the last sprites
       (the credits: a still BG, the cast and names as sprites - 224 line signatures a picture were
       half its time) */
    uint32_t fkey = 0, fast = 0;
    int done = 0, deferred = 0;
    if (!FE->m7w && !m7) {
        fkey = ((sig ^ dsig * 31u) * 0x01000193u ^ FE->lsum) * 0x01000193u;
        for (int i = 0; i < 8; i++) if (!FE->gplane || (i & 3) != 1) fkey = (fkey ^ FE_SCRL[0][i]) * 0x01000193u;
        for (int i = 0; i < 4; i++) fkey = (fkey ^ blkm[i]) * 0x01000193u;
        fkey = (fkey ^ (uint32_t)(perline | obj << 1 | (blk != 0) << 2 | lchg << 3 | FE->subm << 4)) * 0x01000193u;
        for (int i = 0; i < 64; i += 4) fkey = (fkey ^ *(const uint32_t *)&FE_TRK->rgen[i]) * 0x01000193u;
        fast = FE->nfr >= 2 && !FE->hold && !lchg && FE_FOK[bi] && FE_FKEY[bi] == fkey;
        FE_FOK[bi] = 0;                   /* (set again below if this drawing completes) */
    }
    if (fast) {
        for (int y = 0; y < SCREEN_H; y++) {
            int32_t xa = mxa[y >> 3], xb = mxb[y >> 3];
            if (xa >= xb) continue;
            if (y >= FE_CY0 && y < FE_CY0 + FE_CN && (FE->cval[bi][y >> 5] & bit32[y & 31])) {
                fe_restore(y, mcol[y >> 3]);
#ifdef FEDBG
                fedbg_copy++;
#endif
                continue;
            }
            if (y >= FE_CY0 && y < FE_CY0 + FE_CN) { fe_line_clean(y, bi, obj); continue; }
            FE->xa = xa; FE->xb = xb;
            fe_line(y, FB8 + FB_BASE + y * CV_P + CV_XOFF + 32, obj);
            FE->xa = 0; FE->xb = 256;
            FE->nlines++;
#ifdef FEDBG
            fedbg_part++;
#endif
        }
        FE_FOK[bi] = 1;
        FEDBG_MARK(1);
        goto lines_done;
    }
    /* lines whose inputs changed, from where the last frame stopped, within a time budget (a picture
       that changes everywhere - the logos' zoom - is finished over the next frames; the game keeps its
       pace); lines under the last sprites always (their restore) */
    uint32_t v0 = vcount;
#ifndef FEHOST
    uint16_t ft0 = 0;                     /* (I11: the budget of the lines that only scrolled sideways - from
                                             after the signature pass) */
    uint32_t vs0 = 0;
#define FE_SCROLL_BUDGET() (frt_ok ? frt_since(ft0, vs0) > FRAME_TICKS / 4 : vcount - vs0 >= 1)   /* (no FRT: PicoDrive) */
#else
    extern int fe_host_budget;            /* (host test: a line count) */
#define FE_SCROLL_BUDGET() ((int)FE->nlines >= fe_host_budget)
#endif
    FE->xa = 0; FE->xb = 256;
#ifdef FEDBG
    static uint32_t dbg_v0, dbg_v1, dbg_v2;
    dbg_v0 = v0;
    for (int y = 0; y < SCREEN_H; y++) {  /* (test: the redraw marks of this buffer's last picture off) */
        volatile uint32_t *d = FB32 + (FB_BASE + y * CV_P + CV_XOFF) / 4;
        d[74] = d[75] = FE_BORDER4;
    }
#endif
    FEDBG_MARK(1);
    int y0 = FE->ynext;
    og_rot++;
    int nofit = 0;                        /* (no time budget this picture: few lines changed) */
    /* (I11) a BG that only moved sideways a little (Cranky's clouds, 1 px every 4 frames: 72 lines of
       three layers ~ 5 frames of work, the picture held meanwhile = the cabins' stutter): its lines are
       drawn as time allows over the next pictures (1 px behind for a frame - not seen), the sprites and
       text keep their pace */
    int small = 0;
    if (!FE->m7w && FE->nfr < 2)          /* (a new screen: no line "only scrolled") */
        for (int y = 0; y < SCREEN_H; y++) FE_LSIG2[0][y] = FE_LSIG2[1][y] = 0;
    if (!FE->m7w && !m7) {
        uint32_t md = 0;
        for (int i = 0; i < 3; i++) {
            if (i == 1 && FE->gplane) continue;
            uint32_t d = (FE_HOFS(i) - FE_HPREV[i]) & 0x3FF;
            if (d > 512) d = 1024 - d;
            if (d > md) md = d;
#ifdef FEDBG
#endif
            FE_HPREV[i] = FE_HOFS(i);
        }
        small = md <= 4 && FE->nfr >= 2;
#ifdef FEDBG
        fedbg_md = md; fedbg_small = small;
#endif
    }
    {
#if defined(FEDBG) && !defined(FEHOST)
        uint16_t dbg_t0 = frt_read();
#endif
        int n = 0;
        const uint32_t *lsc = FE_LSC;
        const uint16_t *lsc2 = FE_LSC2;
        fe_lsig_all(sig, dsig, perline, m7, blk);
        for (int y = 0; y < SCREEN_H; y++)
            if (lsc[y] != FE_LSIG[bi][y])
                n += !small || lsc2[y] != FE_LSIG2[bi][y];
        nofit = n <= 120;                 /* (~6-8 frames at most: one consistent picture, no dissolve) */
#ifndef FEHOST
        vs0 = vcount; ft0 = frt_read();
#endif
#if defined(FEDBG) && !defined(FEHOST)
        fedbg_tsig += (uint16_t)(frt_read() - dbg_t0);
        fedbg_n = n;
#endif
#ifdef FEHOST
        if (getenv("FESMALL")) printf("  fe small %d n %d h %d %d %d\n", small, n, FE_HOFS(0), FE_HOFS(1), FE_HOFS(2));
#endif
    }
    if (FE->nfr < 2) {                    /* (the sides of a new screen's buffers: black) */
        for (int y = 0; y < SCREEN_H; y++) {
            volatile uint32_t *d = FB32 + (FB_BASE + y * CV_P + CV_XOFF) / 4;
            for (int c = 0; c < 8; c++) { d[c] = FE_BORDER4; d[72 + c] = FE_BORDER4; }
        }
    }
    for (int i = 0; i < SCREEN_H; i++) {
        int j = y0 + i;
        if (j >= SCREEN_H) j -= SCREEN_H;
        /* (I12) in order (was scattered, j * 67 mod 224 - H7 / H8: a change finished over several pictures
           dissolved in instead of showing bands): the BG characters stay in the cache for the next line, the
           scattered order read them from the cartridge again for every line; a picture out of time is held
           unshown since I5 (no bands to see) */
        int y = j;
        uint32_t ls = FE_LSC[y];
        int must = mxa[y >> 3] < mxb[y >> 3];
        if (og && ls == FE_LSIG[bi][y] && (must || !((y + (og_rot >> 1)) & 3))) {
            /* (I5) its "brighter of the two" colours changed level since - (I12) a quarter of these lines a
               picture (the Nintendo logo's glow: 60-80 lines every frame on top of the shine's, ~4 fps) */
            const uint32_t *o = FE_OVF[y];
            uint32_t dg = FE_DGEN[bi][y];
            for (int k = 0; k < 4 && ls == FE_LSIG[bi][y]; k++)
                for (uint32_t w = o[k]; w; w &= w - 1) {
                    uint32_t c = k * 32 + __builtin_ctz(w);
                    if ((uint16_t)(FE_LGEN[c] - dg) < 0x8000 && FE_LGEN[c] != dg) { ls ^= 0x5A5A5A5Au; break; }
                }
        }
        if (ls == FE_LSIG[bi][y] && !must) continue;
        if (ls == FE_LSIG[bi][y]) {
            /* unchanged under the last sprites: only their columns (canvas column c = (32 + x) >> 3 ->
               SNES x 8 (c - 4)), from the clean copy or redrawn - the BG chars come from the cartridge (slow) */
            int32_t xa = mxa[y >> 3], xb = mxb[y >> 3];
            if (xa >= xb) continue;
            if (y >= FE_CY0 && y < FE_CY0 + FE_CN && (FE->cval[bi][y >> 5] & bit32[y & 31])) {
                fe_restore(y, mcol[y >> 3]);
#ifdef FEDBG
                fedbg_copy++;
#endif
                continue;
            }
            if (y >= FE_CY0 && y < FE_CY0 + FE_CN) { fe_line_clean(y, bi, obj); continue; }
            FE->xa = xa; FE->xb = xb;
            fe_line(y, FB8 + FB_BASE + y * CV_P + CV_XOFF + 32, obj);
            FE->xa = 0; FE->xb = 256;
            FE->nlines++;
#ifdef FEDBG
            fedbg_part++;
#endif
            continue;
        }
        if ((y & 1) && (FE_L8[L_MODE][y] & 7) == 7 && (FE_L8[L_MODE][y - 1] & 7) == 7 && !must) {
            FE_LSIG[bi][y] = ls;              /* (mode 7: odd lines show the even line above - half the work) */
            FE->cval[bi][y >> 5] &= ~bit32[y & 31];
            continue;
        }
        if (!m7 && !FE->m7w && ls == FE_LSC[y] && FE_LSCA[y] == FE_LSIGA[bi][y]) {
            /* (I11) only map cells written (the cabin's text typed in): only their columns - the line's other
               pixels are as drawn; under the last sprites the clean copy (updated first) restores theirs */
            int clean = y >= FE_CY0 && y < FE_CY0 + FE_CN && (FE->cval[bi][y >> 5] & bit32[y & 31]);
            int32_t pxa, pxb;
            if ((!must || clean) && fe_dirty_range(y, bi, &pxa, &pxb)) {
                FE->xa = pxa; FE->xb = pxb;
                fe_line(y, FB8 + FB_BASE + y * CV_P + CV_XOFF + 32, obj);
                FE->xa = 0; FE->xb = 256;
                if (clean) {
                    volatile uint32_t *c = FE_CLEAN(y);
                    const uint32_t *l = (const uint32_t *)(FE_LB + 8);
                    for (int i = pxa >> 2; i < pxb >> 2; i++) c[i] = l[i];
                }
                FE_LSIG[bi][y] = ls;
                FE_LSIG2[bi][y] = FE_LSC2[y];
                if (!obj) fe_dh_fix(bi, y, sdk, 1);
                if (og) FE_DGEN[bi][y] = (uint16_t)og;
                FE->nlines++;
#ifdef FEDBG
                fedbg_part++;
#endif
#ifdef FEHOST
                if (getenv("FEPART")) printf("  part y %d x %d-%d\n", y, pxa, pxb);
#endif
                if (must) fe_restore(y, mcol[y >> 3]);
                continue;
            }
        }
        if (small && FE_SCROLL_BUDGET() && FE_LSC2[y] == FE_LSIG2[bi][y]) {
            /* (I11) only scrolled sideways, out of this picture's time: later (under the last sprites
               the old clean copy, if there is one) */
            if (!deferred && !done) FE->ynext = j;   /* (the next picture continues from here) */
#ifdef FEDBG
            fedbg_def++;
#endif
            deferred = 1;
            if (!must) continue;
            if (y >= FE_CY0 && y < FE_CY0 + FE_CN && (FE->cval[bi][y >> 5] & bit32[y & 31])) {
                volatile uint32_t *c = FE_CLEAN(y), *d = (volatile uint32_t *)(FB8 + FB_BASE + y * CV_P + CV_XOFF + 32);
                for (int i = 0; i < 64; i++) d[i] = c[i];
                continue;
            }
        }
#ifdef FE_LINE_BUDGET
        int over = done || (int)FE->nlines >= FE_LINE_BUDGET;   /* (host test: a line budget) */
#else
        int over = !nofit && FE->hold < 4 && (FE_L8[L_MODE][y] & 7) != 7 && FE->nfr >= 2 && (done || vcount - v0 >= 2);   /* (mode 7, a new screen: whole pictures) */
#endif
        if (over && must && y >= FE_CY0 && y < FE_CY0 + FE_CN && (FE->cval[bi][y >> 5] & bit32[y & 31])) {
            /* changed, but out of time: the old clean line under the last sprites (redrawn later) */
            volatile uint32_t *c = FE_CLEAN(y), *d = (volatile uint32_t *)(FB8 + FB_BASE + y * CV_P + CV_XOFF + 32);
            for (int i = 0; i < 64; i++) d[i] = c[i];
            must = 0;
        }
        if (over && !must) {
            if (!done) { done = 1; FE->ynext = j; }
            continue;
        }
#ifdef FEHOST
        if (getenv("FEPART")) { int32_t a_, b_; printf("  full y %d a %d dirty %d must %d\n", y, FE_LSCA[y] == FE_LSIGA[bi][y], fe_dirty_range(y, bi, &a_, &b_), must); }
#endif
        FE_LSIG[bi][y] = FE_LSC[y];
        if (!FE->m7w && !m7) { FE_LSIG2[bi][y] = FE_LSC2[y]; FE_LSIGA[bi][y] = FE_LSCA[y]; }
        if (og) FE_DGEN[bi][y] = (uint16_t)og;
        FE->nlines++;
        fe_line(y, FB8 + FB_BASE + y * CV_P + CV_XOFF + 32, obj);
        if (!obj) fe_dh_fix(bi, y, sdk, !FE->m7w && !m7);
#ifdef FEDBG
        fedbg_full++;
        {   /* (test: right border x 296-303 white = this line was drawn in full this picture) */
            volatile uint32_t *d = FB32 + (FB_BASE + y * CV_P + CV_XOFF) / 4;
            d[74] = d[75] = 0xFFFFFFFFu;      /* (CRAM 255 white: once a picture, fe_palette - a palette write
                                                  waits for the blank in ares) */
        }
#endif
        if (y >= FE_CY0 && y < FE_CY0 + FE_CN) {
            if (obj) {                        /* (the clean copy for the restores under sprites) */
                volatile uint32_t *c = FE_CLEAN(y);
                const uint32_t *l = (const uint32_t *)(FE_LB + 8);
                for (int i = 0; i < 64; i++) c[i] = l[i];
                FE->cval[bi][y >> 5] |= bit32[y & 31];
            } else FE->cval[bi][y >> 5] &= ~bit32[y & 31];
        }
    }
    if (!FE->m7w && !m7 && !done && !deferred) { FE_FKEY[bi] = fkey; FE_FOK[bi] = 1; }
lines_done:
#ifdef FE_NOHOLD
    if (0) {
#else
    if (done) {
#endif
        /* (I5) out of time: this buffer is not shown half new, half old (the shine's slices, the
           cross-fade "by parts") - it is finished in the next calls while the old picture stays up;
           after 4 such calls the rest is drawn without a time limit */
        FE->hold++;
        FE_WAIT_VBL();
        fe_palette();                     /* (the colours keep moving on the picture shown: fades, the flash) */
        return;
    }
    FE->hold = 0;
    {   /* (I11) this buffer is finished: the map cells written since are the next picture's */
        uint32_t *dm = FE_DIRTY + bi * 128;
        for (int i = 0; i < 128; i++) dm[i] = 0;
    }
    for (int y = 0; y < SCREEN_H; y++) {  /* (the line table: mode 7 line doubling) */
        int yy = (y & 1) && (FE_L8[L_MODE][y] & 7) == 7 && (FE_L8[L_MODE][y - 1] & 7) == 7 ? y - 1 : y;
        FB16[y] = (uint16_t)((FB_BASE + yy * CV_P + CV_XOFF) >> 1);
    }
    FE->nfr++;
#ifdef FEDBG
    dbg_v1 = vcount;
    FEDBG_MARK(2);
#endif
    if (obj) {
        spr_clear_marks(bi);
        {   /* sprites on the subscreen added to BG1 (the credits' names: behind BG1 on the main
               screen, shown by colour math) - drawn in front instead (no per-pixel sums here) */
            extern int spr_front_all;
            uint32_t yl = SCREEN_H - 1;
            spr_front_all = (FE_L8[L_TS][yl] & 0x10) && (FE_L8[L_CGA][yl] & 0x01) && !(FE_L8[L_CGA][yl] & 0x80) && (FE_L8[L_CGW][yl] & 2);
        }
#ifndef FEHOST
        {   /* (I11) the sprites: the bottom part by the slave (idle on these screens; its job 2 draws only
               sprites - the lent tile cache memory is not touched), split where half their cells are */
            extern int spr_balance_split(void);
            int32_t split = spr_balance_split();
            shared_t *shs = UNCACHED(&shared_state);
            uint16_t tk = 0;
            if (split > 0 && split < SCREEN_H) {
                shs->sgx = 32; shs->sgy = 0; shs->sbi = bi; shs->ssplit = split; shs->lt_do = 0;
                shs->job = 2;
                tk = ++job_token;             /* (I13: main.c's token counter - see job_token) */
                COMM(C_TOKEN) = tk;
            } else split = SCREEN_H;
            spr_draw_band(0, split, bi, 32, 0);
            if (split < SCREEN_H) {
                while (COMM(C_DONE) != tk)
                    ;
                shs->job = 0;
            }
        }
#else
        spr_draw_band(0, SCREEN_H, bi, 32, 0);
#endif
        { extern int spr_front_all; spr_front_all = 0; }
        /* sprites at SNES x >= 256 or < 0 (the credits' cast walking in): not in the sides */
        for (int r = 0; r < SCREEN_H / 8; r++) {
            if (!(mk[r][0] & 0x0Fu) && !(mk[r][1] & 0xFF0u)) continue;   /* (canvas columns 0-3, 36-43) */
            for (int y = r * 8; y < r * 8 + 8; y++) {
                volatile uint32_t *d = FB32 + (FB_BASE + y * CV_P + CV_XOFF) / 4;
                for (int c = 0; c < 8; c++) { d[c] = FE_BORDER4; d[72 + c] = FE_BORDER4; }
            }
        }
    }
#ifdef FEDBG
    dbg_v2 = vcount;
    FEDBG_MARK(3);
#endif
    spr_pal_dirty = 0; spr_bgpal_dirty = 0; pending_pal = 0;
#ifdef FEDBG
    {   /* (test) bars in the left border (x 0-31, outside the SNES picture: no line is redrawn for them),
           line 1 + 2k = value k: vblanks / picture x 8, game frames x 8, lines drawn / 4, full / 4,
           partial / 4, clean copies / 4, setups, fe_line ticks (x 16 a vblank), signature ticks (x 16),
           changed lines / 4, deferred / 4, then the FRT phases x 16 a vblank: last present -> fe_frame
           (game frames, fe_find), -> the line loop, the loop, the sprites, the rest, the last present */
        static uint32_t vc0, gf0;
        uint32_t dv = vcount - vc0, dg = game_stats.frames - gf0;
        vc0 = vcount; gf0 = game_stats.frames;
        uint32_t v[17] = { dv / 2, dg / 2, FE->nlines / 16, fedbg_full / 4, fedbg_part / 4, fedbg_copy / 4, fedbg_setup,
                           fedbg_tline / 2 / FRAME_TICKS, fedbg_tsig * 16 / FRAME_TICKS, fedbg_n / 4, fedbg_def / 4,
                           fedbg_h[0] / 4, fedbg_h[1] / 4, fedbg_h[2] / 4, fedbg_h[3] / 4, fedbg_h[4] / 4, fedbg_h[5] / 4 };
        for (int k = 0; k < 17; k++) {
            volatile uint8_t *l = FB8 + FB_BASE + (1 + 2 * k) * CV_P + CV_XOFF;
            for (int x = 0; x < 32; x++) l[x] = x < (int)v[k] ? 255 : FE_BORDER;
        }
        fedbg_full = fedbg_part = fedbg_copy = 0;
        fedbg_tline = fedbg_tsig = 0;
        fedbg_def = 0;
        CRAM[255] = 0xFFFF;
    }
#endif
#ifdef SRAMTEST
    {   /* (test: line 7 white = a valid save was loaded from the cartridge) */
        volatile uint8_t *l = FB8 + FB_BASE + 7 * CV_P + CV_XOFF + 32;
        for (int x = 0; x < 128; x++) l[x] = sram_ok ? 255 : FE_BORDER;
        FE_LSIG[bi][7] = 0;
        CRAM[255] = 0xFFFF;
    }
#endif
#ifdef FEDBG2
    {   /* (test: the left border shows each line's MODE TM TS CGA as bits, white = 1; x 0-31) */
        for (int y = 0; y < SCREEN_H; y++) {
            volatile uint8_t *l = FB8 + FB_BASE + y * CV_P + CV_XOFF;
            uint32_t v = FE_L8[L_MODE][y] << 24 | FE_L8[L_TM][y] << 16 | FE_L8[L_TS][y] << 8 | FE_L8[L_CGA][y];
            for (int b = 0; b < 32; b++) l[b] = (v >> (31 - b)) & 1 ? 255 : FE_BORDER;
            l[0] = (FE_LSIG[bi][y] & 1) ? 255 : FE_BORDER;
        }
    }
#endif
    PATHLOG(7, FE->nlines | FE->nfr << 16);
#if defined(FEDBG) && !defined(FEHOST)
    {
        uint16_t t4 = frt_read();
        fedbg_h[0] = (uint16_t)(fedbg_tk[0] - fedbg_tp) * 64u / FRAME_TICKS;
        fedbg_h[1] = (uint16_t)(fedbg_tk[1] - fedbg_tk[0]) * 64u / FRAME_TICKS;
        fedbg_h[2] = (uint16_t)(fedbg_tk[2] - fedbg_tk[1]) * 64u / FRAME_TICKS;
        fedbg_h[3] = (uint16_t)(fedbg_tk[3] - fedbg_tk[2]) * 64u / FRAME_TICKS;
        fedbg_h[4] = (uint16_t)(t4 - fedbg_tk[3]) * 64u / FRAME_TICKS;
        present(FE->gplane ? (int32_t)FE_HOFS(1) - 32 : 0, FE->gplane ? (int32_t)FE_VOFS(1) : 0, frame_no);
        fedbg_tp = frt_read();
        fedbg_h[5] = (uint16_t)(fedbg_tp - t4) * 64u / FRAME_TICKS;
    }
#else
    /* (I11) the Genesis plane's scroll (BG2's; the SNES picture is 32 pixels into the Genesis screen) */
    present(FE->gplane ? (int32_t)FE_HOFS(1) - 32 : 0, FE->gplane ? (int32_t)FE_VOFS(1) : 0, frame_no);
#endif
    fe_palette();
#ifdef FEDBG2
    CRAM[255] = 0xFFFF;
#endif
}
