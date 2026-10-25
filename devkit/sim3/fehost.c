/* host test of sh/fescr.c (the front-end screen renderer) inside the sim: FEHOST=rom.32x FESHOT=f,f,..
   -> /tmp/claude-0/feh/fNNNN.ppm (the 256x224 SNES area as the 32X would show it) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game.h"
#define TCACHE
#define SCREEN_H 224
#define FB_BASE 0x200
#define CV_P 352
#define CV_XOFF 64
#define UNCACHED(p) (p)
static uint8_t fbm2[2][0x20000] __attribute__((aligned(4)));   /* (two frame buffers like the 32X: the back one is drawn) */
static int cur_fb;
#define fbm (fbm2[cur_fb ^ 1])
#define FB8 fbm
#define FB16 ((uint16_t *)fbm)
#define FB32 ((uint32_t *)fbm)
static uint16_t cram[256];
#define CRAM cram
static uint16_t sky_ram[SCREEN_H];
static const uint32_t bit32[32] = {
    1u << 0, 1u << 1, 1u << 2, 1u << 3, 1u << 4, 1u << 5, 1u << 6, 1u << 7, 1u << 8, 1u << 9, 1u << 10,
    1u << 11, 1u << 12, 1u << 13, 1u << 14, 1u << 15, 1u << 16, 1u << 17, 1u << 18, 1u << 19, 1u << 20,
    1u << 21, 1u << 22, 1u << 23, 1u << 24, 1u << 25, 1u << 26, 1u << 27, 1u << 28, 1u << 29, 1u << 30, 1u << 31 };
static uint32_t tc_sdata[4096] __attribute__((aligned(16)));
static uint32_t map_mem[2][64 * 64];
void spr_ck_attach(uint32_t *);
typedef struct { int32_t x0, x1, y0, y1; } trect_t;
typedef struct { int32_t valid; trect_t w, hud; } cvbuf_t;
static cvbuf_t cvb[2];
typedef struct { uint32_t epoch; } shared_t;
static shared_t shared_state;
static const uint16_t *pending_pal;
static int cv_x0par, ripple, disp_pending;
static const uint8_t *sft_show;
extern uint16_t spr_pal[128], spr_bgpal[128];
extern volatile uint32_t spr_pal_dirty, spr_bgpal_dirty;
extern uint32_t spr_marks[2][32][2];
static uint16_t *comm_fn(void) { static uint16_t v; v = 0; return &v; }
#define COMM(n) (*comm_fn())
#define C_CMD 0
static uint8_t *packmem, *romimg;
#define PACK ((const uint8_t *)packmem)
#define PK(T, off) ((const T *)(PACK + (off)))
static struct { uint32_t off_fe; } hdr_s, *hdr = &hdr_s;
static void set_bank(uint32_t b) { memcpy(packmem + 0x340000, romimg + b * 0x80000, 0x80000); }
static void spr_clear_marks(int b) { (void)b; }
static void spr_draw_band(int32_t a, int32_t b, int c, int32_t d, int32_t e) { (void)a; (void)b; (void)c; (void)d; (void)e; }
static void present(int32_t a, int32_t b, uint16_t *f) { (void)a; (void)b; (void)f; }
static void wait_flip(void) {}
static uint16_t frt_read(void) { return 0; }
static int frt_ok = 0;
static uint32_t vcount;
static volatile uint32_t s_job_now;
static uint32_t frame_ticks = 57348;
#define FRAME_TICKS frame_ticks
#define FE_U32(x) __builtin_bswap32(x)
#define FE_U16(x) __builtin_bswap16(x)
#define FE_W(x) __builtin_bswap32(x)
#define FE_HI8(w) ((w) & 255)
#define FE_LO8(w) ((w) >> 8)
#define PATHLOG(c, v) ((void)0)
static void render_state_reset(void) {}
#define FE_WAIT_VBL() ((void)0)
#define FEHOST 1
int fe_host_budget = 1 << 30;
#include "/home/claude/dkc32x/sh/fescr.c"

void fehost_init(void)
{
    const char *r = getenv("FEHOST");
    if (!r) return;
    FILE *f = fopen(r, "rb");
    romimg = malloc(16 << 20);
    size_t n = fread(romimg, 1, 16 << 20, f); fclose(f); (void)n;
    packmem = malloc(0x340000 + 0x80000);
    memcpy(packmem, romimg + 0x40000, 0x340000);
    uint32_t v; memcpy(&v, packmem + 48, 4); hdr_s.off_fe = __builtin_bswap32(v);
    fe_lend();
}
void fehost_frame(int f)
{
    if (getenv("FESIG") && packmem) {
        int i = fe_find();
        if (i >= 0 && fe_idx != i) fe_setup(i);
        if (i >= 0) {
            uint32_t lsum = FE->lsum; int obj = 0;
            for (int y = 0; y < SCREEN_H; y++) obj |= FE_L8[L_TM][y];
            fe_obj = (obj & 0x10) != 0;
            printf("SIG f %d scr %d lsum %08X vgen %u sig %08X m7 %d mode0 %d/%d TM %02X/%02X TS %02X CGA %02X inidisp %02X dark %d col %04X h %d %d %d v %d %d %d\n", f, i, lsum, fe_vgen, fe_signature(), FE->m7, FE_L8[L_MODE][0], FE_L8[L_MODE][200], FE_L8[L_TM][0], FE_L8[L_TM][200], FE_L8[L_TS][100], FE_L8[L_CGA][100], game_inidisp, game_dark(), fe_ppu.coldata[0] | fe_ppu.coldata[1] << 5 | fe_ppu.coldata[2] << 10, fe_ppu.hofs[0], fe_ppu.hofs[1], fe_ppu.hofs[2], fe_ppu.vofs[0], fe_ppu.vofs[1], fe_ppu.vofs[2]);
        }
    }
    if (getenv("FEMODES") && packmem && f == atoi(getenv("FEMODES"))) {
        int i = fe_find(); if (i >= 0 && fe_idx != i) fe_setup(i);
        int py = -1; uint32_t pk = 0xFFFFFFFF;
        for (int y = 0; y <= SCREEN_H; y++) {
            uint32_t k = y < SCREEN_H ? (FE_L8[L_MODE][y] | FE_L8[L_TM][y] << 8 | FE_L8[L_TS][y] << 16 | FE_L8[L_CGA][y] << 24) : 0xFFFFFFFE;
            if (k != pk) { if (py >= 0) printf("MODES y %d-%d mode %02X tm %02X ts %02X cga %02X\n", py, y - 1, pk & 255, (pk >> 8) & 255, (pk >> 16) & 255, pk >> 24); py = y; pk = k; }
        }
    }
    if (getenv("FEBG3COV") && packmem) {   /* per line: 8-px columns where BG3 has pixels (bit mask) */
        static uint64_t prev[SCREEN_H]; static int have;
        int i = fe_find(); if (i >= 0 && fe_idx != i) fe_setup(i);
        if (i == 5) {
            long tot = 0, uni = 0, lines = 0, chg = 0;
            for (int y = 0; y < 150; y++) {
                uint8_t buf[280]; memset(buf, 0, sizeof buf);
                fe_layers(buf, 0, 4, y, FE_L8[L_MODE][y], 0, 256);
                uint64_t m = 0; for (int x = 0; x < 256; x++) if (buf[8 + x]) m |= 1ull << (x >> 3);
                if (have && m != prev[y]) { chg++; uni += __builtin_popcountll(m | prev[y]); }
                tot += __builtin_popcountll(m); prev[y] = m; lines++;
            }
            have = 1;
            printf("COV f %d avgcols %.1f changed %ld avg_union_cols %.1f (of 32)\n", f, tot / (double)lines, chg, chg ? uni / (double)chg : 0.0);
        }
    }
    if (getenv("FEWIN") && packmem) {
        printf("WIN f %d W12 %02X W34 %02X WOBJ %02X WH %02X %02X %02X %02X LOG %02X %02X TMW %02X TSW %02X CGW %02X\n", f,
               game_ppu(0x2123), game_ppu(0x2124), game_ppu(0x2125), game_ppu(0x2126), game_ppu(0x2127), game_ppu(0x2128), game_ppu(0x2129),
               game_ppu(0x212A), game_ppu(0x212B), game_ppu(0x212E), game_ppu(0x212F), game_ppu(0x2130));
    }
    if (getenv("FELDIFF") && packmem) {   /* which per-line registers change from frame to frame */
        static uint8_t p8[NL8][SCREEN_H]; static int16_t p16[NL16][SCREEN_H];
        int i = fe_find();
        if (i >= 0) {
            int n8[NL8] = {0}, n16[NL16] = {0};
            for (int k = 0; k < NL8; k++) for (int y = 0; y < SCREEN_H; y++) if (FE_L8[k][y] != p8[k][y]) { n8[k]++; p8[k][y] = FE_L8[k][y]; }
            for (int k = 0; k < NL16; k++) for (int y = 0; y < SCREEN_H; y++) if (FE_L16[k][y] != p16[k][y]) { n16[k]++; p16[k][y] = FE_L16[k][y]; }
            printf("LD f %d scr %d mode %d tm %d ts %d cgw %d cga %d | m7 %d %d %d %d h %d v %d\n", f, i, n8[0], n8[1], n8[2], n8[3], n8[4], n16[0], n16[1], n16[2], n16[3], n16[4], n16[5]);
        }
    }
    if (getenv("FECOUNT") && packmem) {
        if (getenv("FEEVERY") && f % atoi(getenv("FEEVERY"))) return;
        if (getenv("FEBUDGET")) fe_host_budget = atoi(getenv("FEBUDGET"));   /* (a picture every n game frames) */
        int i = fe_find();
        if (i >= 0) { if (fe_idx != i) fe_setup(i);
            static uint32_t pre[SCREEN_H]; int bi = cur_fb ^ 1; for (int y = 0; y < SCREEN_H; y++) pre[y] = FE_LSIG[bi][y];
            uint16_t fn = 0; fe_frame(&fn);
            printf("CNT f %d scr %d lines %u npair %u\n", f, i, FE->nlines, fe_npair);
            if (getenv("FELINES")) { int a = -1; printf("  changed:"); for (int y = 0; y <= SCREEN_H; y++) { int c = y < SCREEN_H && FE_LSIG[bi][y] != pre[y]; if (c && a < 0) a = y; if (!c && a >= 0) { printf(" %d-%d", a, y - 1); a = -1; } } printf("\n"); }
            cur_fb ^= 1; }
        if (!getenv("FESHOT")) return;
        { char key[16]; snprintf(key, sizeof key, ",%d,", f); char buf[4096]; snprintf(buf, sizeof buf, ",%s,", getenv("FESHOT"));
          if (!strstr(buf, key)) return; }
        goto shot;
    }
    const char *l = getenv("FESHOT");
    if (!l || !packmem) return;
    char key[16]; snprintf(key, sizeof key, ",%d,", f);
    char buf[4096]; snprintf(buf, sizeof buf, ",%s,", l);
    if (!strstr(buf, key)) return;
    int i = fe_find();
    printf("FEHOST f %d screen %d nmi %04X\n", f, i, game_wram16(0x1C));
    if (i < 0) return;
    if (fe_idx != i) fe_setup(i);
    uint16_t fn = 0;
    fe_frame(&fn);
shot:;
    printf("FEHOST npair %u pfull %d m7 %d\n", fe_npair, FE->pfull, FE->m7);
    char p[256]; snprintf(p, sizeof p, "/tmp/claude-0/feh/f%04d.ppm", f);
    FILE *o = fopen(p, "wb");
    fprintf(o, "P6 256 224 255\n");
    for (int y = 0; y < 224; y++)
        for (int x = 0; x < 256; x++) {
            uint8_t *sb = fbm2[cur_fb ^ 1];
            uint32_t c = sb[((uint32_t)((uint16_t *)sb)[y] << 1) + 32 + x];
            uint32_t col = c ? cram[c] : cram[0];
            uint8_t rgb[3] = { (uint8_t)((col & 31) << 3), (uint8_t)(((col >> 5) & 31) << 3), (uint8_t)(((col >> 10) & 31) << 3) };
            fwrite(rgb, 1, 3, o);
        }
    fclose(o);
}
