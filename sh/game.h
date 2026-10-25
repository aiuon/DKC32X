/* DKC game logic interface (see game.c) */
#ifndef GAME_H
#define GAME_H
#include <stdint.h>
typedef struct {
    uint32_t frames, err, errpc, dmas, apu_cmds;
} game_stats_t;
extern game_stats_t game_stats;
extern uint8_t wram_lo[0x2000];
extern uint8_t wram_pool[][256];
extern const int game_npool, game_nscratch, game_nring;
extern uint8_t game_inidisp, game_fblank;
/* screen off: forced blank or brightness 0 */
static inline int game_dark(void) { return (game_inidisp & 0x80) || !(game_inidisp & 0x0F); }
extern const uint8_t game_snap[];
void game_init(const uint8_t *snap, uint32_t entrance);
uint32_t game_frame(uint32_t snes_pad);
uint32_t game_wram16(uint32_t a);
int game_in_main_loop(void);
uint32_t game_wai_pc(void);
int game_hdma_sky(uint32_t table, uint16_t *out, int nlines);
int game_beam_lines(int16_t *LR, int32_t xoff, int nlines, int32_t *pax, int32_t *pay);
/* hooks (sprites.c) */
void game_dma_hook(uint32_t dmap, uint32_t bbus, uint32_t src, uint32_t n);
void game_ppu_hook(uint32_t reg, uint32_t v);
uint32_t game_ppu(uint32_t a);
extern int game_sram_dirty;
uint8_t *game_sram(void);
void game_hdma_run(void (*wr)(void *, int, uint32_t, uint32_t), void *ctx, int nlines);
/* the front-end screens (sprites.c): PPU state the 32X draws them from - in memory main.c lends
   while no level is shown (fe_vm != 0): the VRAM mirror (words 7000-7FFF), then the tracker */
typedef struct {
    uint16_t hofs[4], vofs[4];            /* BG scroll */
    int16_t m7[6];                        /* M7A-D, M7X, M7Y */
    int16_t m7h, m7v;                     /* mode 7 scroll */
    uint8_t bglatch, m7latch, coldata[3], pad;
} fe_ppu_t;
typedef struct { fe_ppu_t ppu; uint32_t vmask, vgen;
                 uint8_t rgen[64]; } fe_trk_t;   /* vmask bit k: words 7000 + k * 256.. written; rgen[k]: words
                                                    7000 + k * 64.. changed (I11: the lines showing them redraw) */
extern uint16_t *fe_vm;                   /* (uncached) VRAM 7000-7FFF mirror, or 0 */
extern void (*fe_lend_hook)(void);
#define FE_TRK ((fe_trk_t *)(fe_vm + 4096))
void fe_reg(uint32_t reg, uint32_t v);
#define SNES_B      0x8000
#define SNES_Y      0x4000
#define SNES_SELECT 0x2000
#define SNES_START  0x1000
#define SNES_UP     0x0800
#define SNES_DOWN   0x0400
#define SNES_LEFT   0x0200
#define SNES_RIGHT  0x0100
#define SNES_A      0x0080
#define SNES_X      0x0040
#define SNES_L      0x0020
#define SNES_R      0x0010
#endif
