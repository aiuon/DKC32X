/* host simulation of the 32X game runtime (sh/game.c + generated code + mkgame tables) */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "game.h"
extern uint32_t sim_cur_f;
extern uint32_t mrd8(uint32_t);
static void simhook(void) { static uint32_t lf = 0xFFFFFFFF; if (lf != game_stats.frames && getenv("HOOKLOG")) { lf = game_stats.frames; printf("HOOK f %u 51A %02X inidisp %02X 3E %04X\n", game_stats.frames, game_wram16(0x51A) & 0xFF, game_inidisp, game_wram16(0x3E)); } }
#include "sprites.h"
uint16_t fbmem[0x10000];
uint32_t tiles_ram[86080 / 4];
uint32_t ring_ent[32 * 64];   /* (class-16 WRAM pages: the comp ring, sh/main.c) */
static int init;
int r65_forcei;
extern const uint8_t *const snes_pg[4096];
#ifdef ROMB_CHECK
extern const uint8_t zero_page[1024];
#ifdef WH_CHECK
extern uint8_t *const wram_hp[512]; extern uint8_t wram_dummy[256];
static uint8_t wh_priv[512][256]; static uint8_t wh_seen[512];
uint8_t *wh_chk(uint32_t a)
{
    uint32_t p = (a >> 8) & 511;
    if (wram_hp[p] != wram_dummy) {
        uint8_t *q = &wram_hp[p][a & 0xFF];
        static uint8_t sc_seen[512];
        if ((uint8_t *)q >= (uint8_t *)tiles_ram && (uint8_t *)q < (uint8_t *)tiles_ram + sizeof tiles_ram && !game_fblank && game_in_main_loop() && !sc_seen[p]) {
            extern volatile uint32_t r65_dbg_key, r65_dbg_pc; sc_seen[p] = 1;
            printf("  SCRATCH page %02X:%02X00 used in gameplay f %u key %08X pc %06X\n", 0x7E + (p >> 8), p & 0xFF, game_stats.frames, r65_dbg_key, r65_dbg_pc);
        }
        return q;
    }
    if (!wh_seen[p]) { wh_seen[p] = 1; extern volatile uint32_t r65_dbg_key; printf("  dummy WRAM page %02X:%02X00 first use f %u key %08X\n", 0x7E + (p >> 8), p & 0xFF, game_stats.frames, r65_dbg_key); }
    return &wh_priv[p][a & 0xFF];
}
#endif
const uint8_t *romb_chk(uint32_t o)
{
    static uint8_t seen[4096];
    o &= 0x3FFFFF;
    const uint8_t *p = &snes_pg[o >> 10][o & 0x3FF];
    if (getenv("RDWATCH")) {
        static uint32_t lo, hi, seen[256]; static int ns;
        if (!hi) sscanf(getenv("RDWATCH"), "%x-%x", &lo, &hi);
        if (o >= lo && o < hi) {
            extern volatile uint32_t r65_dbg_pc;
            int k; for (k = 0; k < ns && seen[k] != r65_dbg_pc; k++);
            if (k == ns && ns < 256) { seen[ns++] = r65_dbg_pc; fprintf(stderr, "RD %06X pc %06X f %u\n", o, r65_dbg_pc, game_stats.frames); }
        }
    }
    if (getenv("PAGELOG")) {   /* PAGELOG=file: every ROM page read (mkgame profile format 'R ppp') */
        static FILE *pf; static uint8_t pseen[4096];
        if (!pf) pf = fopen(getenv("PAGELOG"), "a");
        static int f0 = -1, f1 = -1;
        if (f0 < 0) { f0 = getenv("PAGELOG_F0") ? atoi(getenv("PAGELOG_F0")) : 0; f1 = getenv("PAGELOG_F1") ? atoi(getenv("PAGELOG_F1")) : 1 << 30; }
        if (pf && !pseen[o >> 10] && (int)game_stats.frames >= f0 && (int)game_stats.frames < f1) { pseen[o >> 10] = 1; fprintf(pf, "R %03X\n", o >> 10); fflush(pf); }
    }
    {   /* all-worlds ROM check: WORLDBANK=i (the world bank the 32X would show in window 6) - reads
           of pages that are neither resident nor in that bank would read the wrong data there */
        extern const uint8_t game_pg_world[4096] __attribute__((weak));
        static int wb = -2;
        if (wb == -2) wb = getenv("WORLDBANK") ? atoi(getenv("WORLDBANK")) : -1;
        static uint8_t wseen[4096];
        if (wb >= 0 && game_pg_world) {
            uint32_t g = game_pg_world[o >> 10];
            if (g && !(g & 0x80) && !(g & (1u << wb)) && !wseen[o >> 10] && (int)game_stats.frames >= (getenv("WORLDBANK_F0") ? atoi(getenv("WORLDBANK_F0")) : 0)) {
                extern volatile uint32_t r65_dbg_key, r65_dbg_pc;
                wseen[o >> 10] = 1;
                fprintf(stderr, "WORLD page %03X (rom %06X) banks %02X, not in bank %d: key %08X pc %06X f %u\n",
                        o >> 10, o, g, wb, r65_dbg_key, r65_dbg_pc, game_stats.frames);
            }
        }
    }
    if (snes_pg[o >> 10] == zero_page && !seen[o >> 10]) {
        extern volatile uint32_t r65_dbg_key, r65_dbg_pc;
        seen[o >> 10] = 1;
        fprintf(stderr, "MISSING page %03X (rom %06X) key %08X pc %06X f %u\n", o >> 10, o, r65_dbg_key, r65_dbg_pc, game_stats.frames);
    }
    return p;
}
#endif
#ifdef R65_ITRACE
extern uint32_t r65_a, r65_x, r65_y;
void r65_itrace(uint32_t pc)
{
    pc &= 0xFFFFFF;
    if (getenv("WATCHSPR")) {
        extern uint8_t wram_lo[];
        static uint8_t prev[52]; static uint32_t ppc;
        for (int i = 0; i < 52; i += 2)
            if (!(prev[i] | prev[i + 1]) && (wram_lo[0xD45 + i] | wram_lo[0xD46 + i]))
                { extern uint32_t r65_s; printf("   SPAWN slot %d id %02X%02X by pc %06X f %u x %d stk", i / 2, wram_lo[0xD46 + i], wram_lo[0xD45 + i], ppc, game_stats.frames, wram_lo[0xB19 + i] | wram_lo[0xB1A + i] << 8);
                  for (int k = 1; k <= 12; k++) printf(" %02X", wram_lo[(r65_s + k) & 0x1FFF]); printf("\n"); }
        for (int i = 0; i < 52; i += 2)
            if ((prev[i] | prev[i + 1]) && !(wram_lo[0xD45 + i] | wram_lo[0xD46 + i])) {
                extern uint32_t r65_s;
                int cx = wram_lo[0x88B] | wram_lo[0x88C] << 8, sx = wram_lo[0xB19 + i] | wram_lo[0xB1A + i] << 8;
                printf("   KILL slot %d id %02X%02X by pc %06X f %u rel %d stk", i / 2, prev[i + 1], prev[i], ppc, game_stats.frames, sx - cx);
                for (int k = 1; k <= 9; k++) printf(" %02X", wram_lo[(r65_s + k) & 0x1FFF]); printf("\n");
            }
        memcpy(prev, wram_lo + 0xD45, 52); ppc = pc;
        return;
    }
    if (pc == 0xBBA9B4 || pc == 0xBBA9A3) {
        extern uint8_t wram_lo[];
        printf("   T %06X f %u a %04X x %04X y %04X DP50:", pc, game_stats.frames, r65_a, r65_x, r65_y);
        for (int i = 0x50; i < 0x60; i++) printf(" %02X", wram_lo[i]);
        printf("\n");
    }
}
#endif
static uint32_t script_pad(const char *fn)
{
    static FILE *fp; static int left; static uint32_t cur;
    if (!fp) fp = fopen(fn, "r");
    while (!left) {
        char l[256], b[200] = ""; int n;
        if (!fp || !fgets(l, sizeof l, fp)) return 0;
        if (sscanf(l, "run %d %199s", &n, b) < 1) continue;
        left = getenv("SCALE") ? (int)(n * atof(getenv("SCALE")) + 0.5) : n; if (!left) left = 1; cur = 0;
        if (strstr(b, "RIGHT")) cur |= SNES_RIGHT; if (strstr(b, "LEFT")) cur |= SNES_LEFT;
        if (strstr(b, "UP")) cur |= SNES_UP; if (strstr(b, "DOWN")) cur |= SNES_DOWN;
        if (strstr(b, "Y")) cur |= SNES_Y; if (strstr(b, "B")) cur |= SNES_B;
        if (strstr(b, "A")) cur |= SNES_A; if (strstr(b, "X")) cur |= SNES_X;
        if (strstr(b, "START")) cur |= SNES_START;
    }
    left--; return cur;
}
#include <string.h>
int main(int argc, char **argv)
{
    int ent = argc > 1 ? strtol(argv[1], 0, 16) : 0x16, n = argc > 2 ? atoi(argv[2]) : 1000;
    {   /* the level-start snapshot (mkgame.py: build/game/snap_EE.bin) */
        static uint8_t snapbuf[1 << 20];
        char sp[256]; snprintf(sp, sizeof sp, "/home/claude/dkc32x/build/game/snap_%02X.bin", ent);
        if (getenv("SNAPFILE")) snprintf(sp, sizeof sp, "%s", getenv("SNAPFILE"));
        FILE *sf = fopen(sp, "rb");
        if (!sf) { printf("no snapshot %s\n", sp); return 1; }
        size_t sn = fread(snapbuf, 1, sizeof snapbuf, sf); fclose(sf); (void)sn;
        game_init(snapbuf, ent);
    }
    if (getenv("CKCHK")) { static uint32_t ckbuf[512 * 8]; extern void spr_ck_attach(uint32_t *); spr_ck_attach(ckbuf); }
    int nreload = 0;
    { extern void fehost_init(void); fehost_init(); }
    if (getenv("FEVM")) { extern uint16_t *fe_vm; static uint16_t fevm[8192]; fe_vm = fevm; }
    { extern void (*fe_lend_hook)(void); fe_lend_hook = simhook; }
    if (getenv("JUMP0")) { extern void game_debug_jump(uint32_t); game_debug_jump(strtoul(getenv("JUMP0"), 0, 16)); }
    r65_forcei = getenv("FORCEI") != 0;
    for (int f = 1; f <= n; f++) {
        uint32_t sp = SNES_RIGHT | SNES_Y;
        if ((f % 90) < 12) sp |= SNES_B;
        if ((f % 600) > 560) sp = SNES_LEFT;
        if (getenv("SCRIPT")) sp = script_pad(getenv("SCRIPT"));
        if (getenv("BOT")) {   /* BOT=botpre: main.c GAMEBOT (+ BOTPRE walk to x, jump, wait) */
            static uint32_t bf, lastx, still, mode, mt, st, t; extern uint8_t wram_lo[];
            uint32_t x = wram_lo[0xB1B] | wram_lo[0xB1C] << 8, pre = atoi(getenv("BOT"));
            bf++;
            if (x == lastx) still++; else { still = 0; lastx = x; }
            if (mt) mt--; else mode = 0;
            if (still > 30 && !mt) { mode = 1 + (bf / 37) % 3; mt = 60; still = 0; }
            sp = SNES_RIGHT; if ((bf % 50) < 6) sp |= SNES_Y;
            if (mode == 1) sp = SNES_UP | SNES_RIGHT | ((bf & 8) ? SNES_B : 0);
            else if (mode == 2) sp = SNES_DOWN | SNES_RIGHT;
            else if (mode == 3) sp = (mt > 30 ? SNES_LEFT : SNES_UP) | ((bf & 8) ? SNES_B : 0);
            if (pre && !game_fblank && game_in_main_loop() && !game_dark()) {
                if (st == 0) { sp = SNES_RIGHT; still = 0; if (x >= pre) { st = 1; t = 0; } }
                else if (st == 1) { t++; sp = t < 12 ? SNES_RIGHT | SNES_B : t < 32 ? SNES_RIGHT : 0; still = 0; if (t >= 130) st = 2; }
            }
        }
#ifdef VRAMLOG
        { extern int vram_log; vram_log = 1; }
#endif
        if (getenv("JUMPF")) {   /* JUMPF=frame:key - the next NMI runs from key instead */
            int jf = 0; unsigned jk = 0; sscanf(getenv("JUMPF"), "%d:%x", &jf, &jk);
            if (f == jf) { extern void game_debug_jump(uint32_t); game_debug_jump(jk); printf("f %d jump %08X\n", f, jk); }
        }
        if (getenv("GOTO")) {   /* GOTO=frame:entrance[,frame:entrance...]: load that entrance (cheat-code path) */
            const char *g = getenv("GOTO");
            while (*g) {
                int gf = atoi(g); const char *c = strchr(g, ':'); if (!c) break;
                int ge = strtol(c + 1, 0, 16);
                if (gf == f) {
                    extern uint8_t wram_lo[]; extern void game_debug_jump(uint32_t);
                    uint16_t w[4] = { 0x3E, 0x40, 0x2E, 0x565 };
                    for (int i = 0; i < 4; i++) { wram_lo[w[i]] = ge & 255; wram_lo[w[i] + 1] = ge >> 8; }
                    game_debug_jump(0x0080806F);
                    printf("f %d GOTO entrance %02X (wai %06X)\n", f, ge, game_wai_pc());
                }
                const char *n = strchr(g, ','); if (!n) break; g = n + 1;
            }
        }
        if (getenv("WS")) {   /* (I21d) WS=L,W: the OBJECT RANGE windows (r65_ws) held at L / W / W - L */
            extern uint32_t r65_ws[3]; int l = 0, w = 0; sscanf(getenv("WS"), "%d,%d", &l, &w);
            r65_ws[0] = l; r65_ws[1] = w; r65_ws[2] = w - l;
        }
        if (getenv("PALOVF")) {   /* (I21d) sprite palette slots: the most in use, every overflow ($1E3D = $78) */
            extern uint8_t wram_lo[]; static int mx;
            int c = 0; for (int k = 0; k < 8; k++) if (wram_lo[0x1AD3 + 2 * k] | wram_lo[0x1AD4 + 2 * k]) c++;
            uint32_t x = wram_lo[0xB1B] | wram_lo[0xB1C] << 8, cx = wram_lo[0x88B] | wram_lo[0x88C] << 8;
            if (c > mx) { mx = c; printf("PAL f %d slots %d kx %u cam %u\n", f, c, x, cx); }
            if (wram_lo[0x1E3D] == 0x78) printf("PAL f %d OVERFLOW pal %04X kx %u cam %u\n", f, wram_lo[0x1E35] | wram_lo[0x1E36] << 8, x, cx);
        }
        if (getenv("TP")) {   /* TP=start,every,dx: move slot-0 sprite (the Kong) right (exploration) */
            int t0 = 0, ev = 60, dx = 128; sscanf(getenv("TP"), "%d,%d,%d", &t0, &ev, &dx);
            if (f >= t0 && (f - t0) % ev == 0) {
                extern uint8_t wram_lo[]; uint32_t x = wram_lo[0xB1B] | wram_lo[0xB1C] << 8; x += dx;
                wram_lo[0xB1B] = x & 255; wram_lo[0xB1C] = x >> 8;
            }
        }
        if (getenv("TPGRID")) {   /* TPGRID=x0,x1,dx,y0,y1,dy,every: visit a grid of Kong positions (free movement) */
            int x0, x1, dx, y0, y1, dy, ev; sscanf(getenv("TPGRID"), "%d,%d,%d,%d,%d,%d,%d", &x0, &x1, &dx, &y0, &y1, &dy, &ev);
            extern uint8_t wram_lo[]; wram_lo[0x535] = 1; wram_lo[0x536] = 0;
            if (f > 30 && (f % ev) == 0) {
                int k = (f - 30) / ev, nx = (x1 - x0) / dx + 1, i = k % nx, j = k / nx;
                int x = x0 + i * dx, y = y0 + j * dy;
                if (y <= y1) { wram_lo[0xB1B] = x & 255; wram_lo[0xB1C] = x >> 8; wram_lo[0xBC3] = y & 255; wram_lo[0xBC4] = y >> 8; }
            }
        }
        if (getenv("POKE")) {   /* POKE=frame:addr:val16[,...] (WRAM lo) */
            const char *g = getenv("POKE");
            while (*g) {
                unsigned pf, pa, pv; if (sscanf(g, "%u:%x:%x", &pf, &pa, &pv) != 3) break;
                if (pf == (unsigned)f || (pf == 0)) { extern uint8_t wram_lo[]; wram_lo[pa] = pv & 255; wram_lo[pa + 1] = pv >> 8; }
                const char *n = strchr(g, ','); if (!n) break; g = n + 1;
            }
        }
        uint32_t e = game_frame(sp);
        { extern void fehost_frame(int); fehost_frame(f); }
        if (getenv("TXTLOG") && f % 30 == 0) {   /* the cabin dialogue (front-end VRAM mirror, FEVM=1) */
            extern uint16_t *fe_vm;
            if (fe_vm) { printf("f %d text:\n", f);
                for (int r = 0; r < 16; r++) { int any = 0; for (int c = 0; c < 32; c++) any |= (fe_vm[0x600 + r * 32 + c] & 0xFF) > 0x20;
                    if (any) { printf("  %2d |", r + 16); for (int c = 0; c < 32; c++) { int ch = fe_vm[0x600 + r * 32 + c] & 0xFF; putchar(ch >= 0x20 && ch < 0x7F ? ch : '.'); } printf("|\n"); } } }
        }
        if (getenv("HOFSLOG") && f % atoi(getenv("HOFSLOG")) == 0) {   /* per-line BG2 / BG3 H scroll writes (HDMA) */
            static int h2[224], h3[224], lo2, lo3;
            extern void game_hdma_run(void (*)(void *, int, uint32_t, uint32_t), void *, int);
            void cb(void *c, int y, uint32_t r, uint32_t v) { if (r == 0x210F) { h2[y] = (h2[y] >> 8) | v << 8; } if (r == 0x2111) { h3[y] = (h3[y] >> 8) | v << 8; } }
            for (int k = 0; k < 224; k++) h2[k] = h3[k] = -1;
            game_hdma_run(cb, 0, 224);
            printf("f %d cam %d BG2 hofs per line:", f, game_wram16(0x88B)); for (int k = 0; k < 224; k += 8) printf(" %d:%d", k, h2[k]); printf("\n");
        }
        if (getenv("BEAMLOG") && f % 25 == 0) {   /* lit lines of the game's window table ($7F1C28) */
            extern uint8_t game_wram_hi_byte(uint32_t);
            int16_t lr[448]; int32_t ax, ay; extern int game_beam_lines(int16_t *, int32_t, int, int32_t *, int32_t *);
            int k = game_beam_lines(lr, 0, 224, &ax, &ay), n = 0;
            for (int y = 0; y < 224; y++) if (lr[2 * y] != 0x7FFF && lr[2 * y] != -0x7FFF) n++; else if (lr[2*y] == -0x7FFF && lr[2*y+1] != -0x7FFF) n++;
            extern uint8_t wram_lo[];
            printf("BEAM f %d kind %d lit %d apex %d,%d x %d\n", f, k, n, ax, ay, wram_lo[0xB1B] | wram_lo[0xB1C] << 8);
        }
        if (getenv("ROPELOG") && f % 10 == 0) {   /* rope tiles ($060) in OAM: x (9-bit), y */
            extern uint8_t wram_lo[]; int n = 0, nw = 0, xs[8]; 
            for (int i = 0; i < 128; i++) { int h = wram_lo[0x400 + (i >> 2)] >> ((i & 3) * 2); int x = wram_lo[0x200 + i * 4] | (h & 1) << 8; int y = wram_lo[0x201 + i * 4];
                if (y < 224 && wram_lo[0x202 + i * 4] == 0x60) { if (n < 8) xs[n] = x; n++; if (x >= 256 && x < 320) nw++; } }
            if (n) { printf("f %d rope tiles %d wide %d x:", f, n, nw); for (int k = 0; k < n && k < 8; k++) printf(" %d", xs[k]); printf("\n"); }
        }
        if (getenv("OAMWIDE") && f % 50 == 0) {   /* OAM entries in the 32X's extra columns (x 256..319) */
            extern uint8_t wram_lo[]; int n = 0, nl = 0;
            for (int i = 0; i < 128; i++) { int h = wram_lo[0x400 + (i >> 2)] >> ((i & 3) * 2); int x = wram_lo[0x200 + i * 4] | (h & 1) << 8; int y = wram_lo[0x201 + i * 4];
                if (y < 224 && x >= 256 && x < 320) n++; if (y < 224 && x >= 448) nl++; }
            printf("f %d oam wide %d left %d\n", f, n, nl);
        }
        if (getenv("SONGLOG")) {   /* the game's music requests ($051D playing, $0523 song) */
            static int ps = -1; int w = game_wram16(0x51D) & 0xFF ? game_wram16(0x523) & 0xFF : 0xFF;
            if (w != ps) printf("f %d song %02X ent %04X\n", f, w, game_wram16(0x3E)); ps = w;
        }
        if (getenv("SPAWNLOG")) {
            static uint16_t pid[26];
            int cx = game_wram16(0x88B);
            for (int i = 0; i < 26; i++) {
                uint16_t id = game_wram16(0xD45 + 2 * i);
                if (id != pid[i]) printf("f %d slot %d id %04X -> %04X x %d (rel %d) y %d anim %04X\n", f, i, pid[i], id,
                    game_wram16(0xB19 + 2 * i), (int16_t)game_wram16(0xB19 + 2 * i) - cx, game_wram16(0xBC1 + 2 * i), game_wram16(0x10D1 + 2 * i));
                pid[i] = id;
            }
        }
        if (getenv("GRPLOG")) { extern uint8_t wram_lo[]; static uint32_t pv = 0xFFFFFFFF;
            uint32_t v = wram_lo[0x192B + 0x8A] | wram_lo[0x192B + 0x8B] << 8 | wram_lo[0x192B + 0x8C] << 16;
            uint32_t kx = wram_lo[0xB1B] | wram_lo[0xB1C] << 8, cx = wram_lo[0x88B] | wram_lo[0x88C] << 8;
            if (v != pv || (getenv("GRPEVERY") && (f % atoi(getenv("GRPEVERY"))) == 0)) { { int fr = 0; for (int k = 2; k < 0x1E; k += 2) fr += !(wram_lo[0xD45 + k] | wram_lo[0xD46 + k]); printf("GRP f %d flags %06X g59 %02X kx %u cam %u ent %02X free %d\n", f, v, wram_lo[0x192B + 0x59], kx, cx, wram_lo[0x3E], fr); } pv = v; } }
        { extern uint32_t hle_slot_tight; extern uint8_t wram_lo[]; if (hle_slot_tight) { hle_slot_tight = 0;
            printf("SLOTBLOCK f %d ent %02X kx %u\n", f, wram_lo[0x3E], wram_lo[0xB1B] | wram_lo[0xB1C] << 8); } }
        spr_prepare();
#ifdef VRAM_SHADOW
        if ((!game_fblank && game_in_main_loop()) || getenv("SPRCHKALL")) { extern void spr_check(uint32_t); spr_check(f); }
#endif
        if (getenv("PALLOG") && f == atoi(getenv("PALLOG"))) {
            extern uint16_t spr_pal[128];
            for (int p = 0; p < 8; p++) { printf("pal %d:", p); for (int i = 0; i < 16; i++) printf(" %04X", spr_pal[p * 16 + i]); printf("\n"); }
            extern uint8_t wram_lo[];
            for (int i = 0; i < 128; i++) if (wram_lo[0x201 + i * 4] < 224) printf("oam %d x %d y %d t %02X a %02X\n", i, wram_lo[0x200 + i * 4], wram_lo[0x201 + i * 4], wram_lo[0x202 + i * 4], wram_lo[0x203 + i * 4]);
        }
        if (getenv("PALCHK") && !game_fblank && game_in_main_loop() && !(game_wram16(0x1E15) & 0x10)) {
            /* (I21d) every loaded sprite palette ($1AD3 slots) against the port's colours (spr_pal):
               a slot that stays different for 4+ frames is reported once per run of frames */
            extern uint16_t spr_pal[128]; static int bad[8], room;
            int r = game_wram16(0x3E);
            if (r != room) { room = r; printf("PALCHK f %d entrance %02X\n", f, r); }
            for (int k = 0; k < 8; k++) {
                uint32_t p = game_wram16(0x1AD3 + 2 * k); int d = 0;
                if (p) for (int j = 0; j < 15; j++) {
                    uint32_t c = mrd8(0xBC0000 + p + 2 * j) | mrd8(0xBC0000 + p + 2 * j + 1) << 8;
                    if (spr_pal[k * 16 + 1 + j] != ((c & 0x7FFF) | 0x8000)) d++;
                }
                if (d) { if (++bad[k] == 4) printf("PALCHK f %d slot %d pal %04X: %d colours differ (refs %d) kx %d\n", f, k, p, d, game_wram16(0x1AE3 + 2 * k), game_wram16(0xB1B)); }
                else { if (bad[k] >= 4) printf("PALCHK f %d slot %d ok again after %d frames\n", f, k, bad[k]); bad[k] = 0; }
            }
        }
        if (getenv("PALUSE") && !game_fblank && game_in_main_loop()) {
            /* (I21d) objects whose palette slot is free, or more users than the slot's count */
            static int rep[8]; int use[8] = {0}; int fr[8] = {0};
            for (int i = 2; i < 26; i++) {
                if (!game_wram16(0xD45 + 2 * i)) continue;
                int sl = (game_wram16(0xC69 + 2 * i) >> 9) & 7;
                use[sl]++;
                if (!game_wram16(0x1AD3 + 2 * sl)) fr[sl]++;
            }
            for (int k = 0; k < 8; k++) {
                int refs = (int16_t)game_wram16(0x1AE3 + 2 * k);
                int b = fr[k] || use[k] > refs;
                if (b && !rep[k]) printf("PALUSE f %d slot %d pal %04X refs %d users %d free-users %d kx %d entrance %02X\n", f, k, game_wram16(0x1AD3 + 2 * k), refs, use[k], fr[k], game_wram16(0xB1B), game_wram16(0x3E));
                rep[k] = b;
            }
        }
        if (getenv("SLOTLOG")) {   /* (I21d) the palette slot table whenever it changes */
            static uint16_t ps[8], pr[8];
            int ch = 0;
            for (int k = 0; k < 8; k++) if (ps[k] != game_wram16(0x1AD3 + 2 * k) || pr[k] != game_wram16(0x1AE3 + 2 * k)) ch = 1;
            if (ch) {
                printf("SLOT f %d:", f);
                for (int k = 0; k < 8; k++) { ps[k] = game_wram16(0x1AD3 + 2 * k); pr[k] = game_wram16(0x1AE3 + 2 * k); printf(" %04X/%d", ps[k], (int16_t)pr[k]); }
                printf("\n");
            }
        }
        if (getenv("PALREUSE") && !game_fblank && game_in_main_loop()) {
            /* (I21d) a live object whose palette slot was given to another palette meanwhile */
            static uint16_t mid[26], mptr[26]; static uint8_t mk[26]; static int nrep;
            for (int i = 2; i < 26; i++) {
                uint16_t id = game_wram16(0xD45 + 2 * i);
                int k = (game_wram16(0xC69 + 2 * i) >> 9) & 7;
                uint16_t p = game_wram16(0x1AD3 + 2 * k);
                if (!id || id != mid[i] || k != mk[i] || !mptr[i]) { mid[i] = id; mk[i] = k; mptr[i] = p; continue; }
                if (p != mptr[i]) {
                    if (nrep++ < 40) printf("PALREUSE f %d obj %d id %04X slot %d pal %04X -> %04X refs %d x %d kx %d entrance %02X\n", f, i, id, k, mptr[i], p,
                        game_wram16(0x1AE3 + 2 * k), game_wram16(0xB19 + 2 * i), game_wram16(0xB1B), game_wram16(0x3E));
                    mptr[i] = p;
                }
            }
        }
        if (getenv("WRAMHASH")) {   /* (I16) per frame: FNV-1a of the same bytes WRAMDUMP writes (regressions) */
            static FILE *hf; if (!hf) hf = fopen(getenv("WRAMHASH"), "w");
            extern uint8_t wram_lo[]; extern uint8_t wram_pool[][256]; extern const int game_npool; extern const int game_nring;
            uint64_t h = 1469598103934665603ULL;
            for (int i = 0; i < 0x2000; i++) { h ^= wram_lo[i]; h *= 1099511628211ULL; }
            for (int i = 0; i < game_npool * 256; i++) { h ^= ((uint8_t *)wram_pool)[i]; h *= 1099511628211ULL; }
            for (int i = 0; i < game_nring * 256; i++) { h ^= ((uint8_t *)ring_ent)[i]; h *= 1099511628211ULL; }
            fprintf(hf, "%d %016llx\n", f, (unsigned long long)h);
        }
        if (getenv("WRAMDUMP")) {
            static FILE *wf; if (!wf) wf = fopen(getenv("WRAMDUMP"), "wb");
            extern uint8_t wram_lo[]; extern uint8_t wram_pool[][256]; extern const int game_npool;
            fwrite(wram_lo, 1, 0x2000, wf); fwrite(wram_pool, 256, game_npool, wf); { extern const int game_nring; fwrite(ring_ent, 256, game_nring, wf); }
        }
        if (getenv("BGPDUMP") && f == atoi(getenv("BGPDUMP"))) {   /* the game's BG colours 0-127 */
            extern uint16_t spr_bgpal[128];
            for (int i = 0; i < 128; i++) printf("%04X%s", spr_bgpal[i] & 0x7FFF, (i & 15) == 15 ? "\n" : " ");
        }
        if (getenv("BGPLOG")) { extern volatile uint32_t spr_bgpal_dirty; extern uint16_t spr_bgpal[128]; if (spr_bgpal_dirty) { printf("bgpal f %d dirty %02X c17 %04X c40 %04X\n", f, spr_bgpal_dirty, spr_bgpal[17], spr_bgpal[40]); spr_bgpal_dirty = 0; } }
        if (getenv("SKYLOG") && f % atoi(getenv("SKYLOG")) == 0) {
            uint16_t sk[224]; int ok = game_hdma_sky(0x1B41, sk, 224);
            extern uint8_t wram_lo[]; printf("wram1B41:"); for (int i = 0; i < 48; i++) printf(" %02X", wram_lo[0x1B41 + i]); printf("\n");
            { extern const uint8_t *game_regs_dbg(void); }
            printf("sky f %d ok %d:", f, ok); for (int i = 0; i < 224; i += 16) printf(" %04X", sk[i]); printf("\n");
        }
        if (getenv("FECHK") && f % atoi(getenv("FECHK")) == 0) {   /* per-line BG modes from the HDMA tables */
            static uint8_t md[224]; static int k;
            extern void game_hdma_run(void (*)(void *, int, uint32_t, uint32_t), void *, int);
            extern uint32_t game_ppu(uint32_t);
            void cb(void *c, int y, uint32_t r, uint32_t v) { if (r == 0x2105) md[y] = v | 0x80; }
            for (k = 0; k < 224; k++) md[k] = 0;
            game_hdma_run(cb, 0, 224);
            printf("f %d 1C %04X cpu mode %02X lines:", f, game_wram16(0x1C), game_ppu(0x2105));
            for (k = 0; k < 224; k++) if (md[k]) printf(" %d:%02X", k, md[k] & 0x7F);
            printf("\n");
        }
        if (getenv("ICOUNT")) { extern uint32_t r65_ibudget; printf("f %d interp insns %u\n", f, r65_ibudget); }
        if (getenv("SIGCHK")) {   /* what changes per frame (front-end redraw signature parts) */
            extern void game_hdma_run(void (*)(void *, int, uint32_t, uint32_t), void *, int);
            extern uint16_t *fe_vm;
            static uint32_t hh, ph, pv, pp[24];
            void cb(void *c, int y, uint32_t r, uint32_t v) { hh = (hh ^ (y << 16 | r << 8 | v)) * 0x01000193u; }
            hh = 0; game_hdma_run(cb, 0, 224);
            if (hh != ph) printf("f %d hdma changed\n", f);
            ph = hh;
            if (fe_vm) { uint32_t *t = (uint32_t *)(fe_vm + 4096); uint16_t *q = (uint16_t *)t;
                for (int i = 0; i < 24; i++) if (q[i] != pp[i]) { printf("f %d trk[%d] %04X -> %04X\n", f, i, pp[i], q[i]); pp[i] = q[i]; } }
        }
        if (getenv("MAPWATCH")) {   /* VRAM words 7000-713F (shadow) changes + game_fblank / dark / main loop per frame */
            extern uint8_t shv[]; static uint32_t ph = 0, pst = 99;
            uint32_t h = 0; for (int i = 0x7000 * 2; i < 0x7140 * 2; i++) h = h * 31 + shv[i];
            uint32_t st = game_fblank | game_dark() << 1 | game_in_main_loop() << 2;
            static uint8_t pv[0x280];
            if (h != ph || st != pst) { printf("f %d MAP %08X fblank %d dark %d main %d 3E %04X changed:", f, h, st & 1, (st >> 1) & 1, st >> 2, game_wram16(0x3E)); int nn = 0; for (int i = 0; i < 0x280 && nn < 6; i++) if (pv[i] != shv[0xE000 + i]) { printf(" %03X:%02X>%02X", i, pv[i], shv[0xE000 + i]); nn++; } printf("\n"); memcpy(pv, shv + 0xE000, 0x280); }
            ph = h; pst = st;
        }
        if (getenv("APULOG")) { extern uint32_t game_apu_ptr, game_apu_n; static uint32_t pn, p51d = 0xFFFF, p523 = 0xFFFF;
            if (game_apu_n != pn || game_wram16(0x51D) != p51d || game_wram16(0x523) != p523) printf("f %d APU upload %06X n %u 51D %04X 523 %04X 1C %04X\n", f, game_apu_ptr, game_apu_n, game_wram16(0x51D), game_wram16(0x523), game_wram16(0x1C));
            pn = game_apu_n; p51d = game_wram16(0x51D); p523 = game_wram16(0x523); }
        if (getenv("WWATCH")) { static uint32_t pv = 0xFFFFFFFF, wa; if (!wa) wa = strtoul(getenv("WWATCH"), 0, 16); uint32_t v = game_wram16(wa); if (v != pv) printf("f %d WW %04X = %04X 1C %04X 3E %04X\n", f, wa, v, game_wram16(0x1C), game_wram16(0x3E)); pv = v; }
        if (getenv("NMILOG")) { static uint32_t pn = 0xFFFFFFFF; uint32_t n = game_wram16(0x1C) | game_wram16(0x3E) << 16; if (n != pn) printf("f %d NMI routine 80%04X wai %06X inidisp %02X ent %04X\n", f, n & 0xFFFF, game_wai_pc(), game_inidisp, game_wram16(0x3E)); pn = n; }
        if (getenv("WAILOG")) { static uint32_t pw; uint32_t w = game_wai_pc(); if (w != pw) printf("f %d WAI %06X inidisp %02X ent %04X\n", f, w, game_inidisp, game_wram16(0x3E)); pw = w; }
        if (!game_fblank) {     /* scratch must not change outside forced-blank sequences */
            static uint32_t chk[86080 / 4];
            if (init && memcmp(chk, tiles_ram, sizeof chk)) { printf("frame %d: scratch written while the screen is on:", f); { int lp=-1; for (unsigned k=0;k<sizeof chk;k++) if (((uint8_t*)chk)[k]!=((uint8_t*)tiles_ram)[k] && (int)(k>>8)!=lp) { lp=k>>8; printf(" s%d", lp);} } printf("\n"); }
            memcpy(chk, tiles_ram, sizeof chk); init = 1;
        }
        if (getenv("SCRIBBLE") && !game_fblank && game_in_main_loop()) {   /* the 32X tile cache overwrites the scratch pages */
            static uint32_t r = 12345; for (unsigned k = 0; k < sizeof tiles_ram / 4; k++) { r = r * 1103515245 + 12345; tiles_ram[k] = r; }
            init = 0;
        }
        if (game_fblank && !game_dark() && game_in_main_loop()) {   /* the 32X rebakes its tile cache here: clobber the scratch */
            game_fblank = 0; memset(tiles_ram, 0xA5, sizeof tiles_ram); nreload++; init = 0;
            { static uint32_t *c; extern int dummy_; }
        }
        { static uint32_t lw; if (spr_stats.wram_dma != lw) { printf("frame %d: OBJ DMA from WRAM (%u)%s\n", f, spr_stats.wram_dma - lw, game_in_main_loop() && !game_dark() ? " IN GAMEPLAY" : ""); lw = spr_stats.wram_dma; } }
        if (f % 100 == 0 || e) { extern volatile uint32_t hle_cam_xmin, hle_cam_xmax, hle_cam_frame; printf("frame %d camx %d err %08X reloads %d cam %d..%d blk %d..%d (%u) y %d\n", f, game_wram16(0x88B), e, nreload, game_wram16(0x1B23), game_wram16(0x1B25), hle_cam_xmin, hle_cam_xmax, hle_cam_frame, game_wram16(0x895)); }
        if (e) return 1;
    }
    return 0;
}
#ifdef IOCOUNT
extern uint32_t io_wcnt[0x10000];
__attribute__((destructor)) static void dump_io(void)
{
    for (int i = 0; i < 0x10000; i++) if (io_wcnt[i]) printf("io %04X %u\n", i, io_wcnt[i]);
}
#endif
