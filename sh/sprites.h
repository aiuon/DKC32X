/* SNES OBJ layer (sprites.c) */
#ifndef SPRITES_H
#define SPRITES_H
#include <stdint.h>
typedef struct { uint32_t drawn, tiles, odd_dma, wram_dma; } spr_stats_t;
extern spr_stats_t spr_stats;
extern uint16_t spr_pal[128];
extern volatile uint32_t spr_pal_dirty;
extern uint16_t spr_bgpal[128];
extern volatile uint32_t spr_bgpal_dirty;
extern uint32_t spr_marks[2][32][2];
void spr_init(const uint16_t *obj_idx, const uint8_t *cgram);
void spr_prepare(void);
void spr_prepare_a(void);              /* (I17) master part: uploads, palettes, the conversion list */
void spr_prepare_b(void);              /* (I17) either CPU: the sprite list + conversions */
void spr_draw_band(int32_t y0, int32_t y1, int bi, int32_t gx, int32_t gy);
void spr_clear_marks(int bi);
int spr_draw_lt(const volatile uint16_t *lt, int16_t (*r)[4], int maxr);
void spr_slave_sync(void);
void spr_clear_rows(int bi, int32_t r0, int32_t r1);
extern uint8_t *spr_rwbuf;
void spr_rw_release(void);
extern uint32_t *spr_ck;
void spr_ck_attach(uint32_t *buf);
#endif
extern const uint8_t *spr_rip_pat;
extern int32_t spr_rip_phase;
