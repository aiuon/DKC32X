/* 32X memory model for the recompiled 65816 code (included by r65rt.h) */
extern uint8_t wram_lo[0x2000] __attribute__((aligned(16)));
extern uint8_t *const wram_hp[512];
extern const uint8_t *const snes_pg[4096];
/* WRAM 0-1FFF through GBR (= wram_lo on both CPUs, set at boot): @(disp,GBR) addressing reaches the
   whole direct page without address constants */
#ifndef R65_NO_GBR
#define WL(a) (((uint8_t *)__builtin_thread_pointer())[(a)])
#define R65_WL_GBR 1
#else
#define WL(a) wram_lo[(a)]
#endif
#ifdef WH_CHECK
uint8_t *wh_chk(uint32_t a);              /* host check: accesses of WRAM pages not in the pool */
#define WH(a) (*wh_chk(a))
#else
#define WH(a) wram_hp[(a) >> 8][(a) & 0xFF]
#endif
#ifdef ROMB_CHECK
const uint8_t *romb_chk(uint32_t o);   /* host check: reads of ROM pages missing from the blob */
#define ROMB(o) (*romb_chk(o))
#else
/* (I13) the page pointer as (o >> 8) & 0x3FFC: 3 SH-2 instructions instead of 6 (no barrel shifter);
   o < 0x400000 always (callers mask to the 4 MB ROM) - the mask also keeps any other value in the table */
#if defined(__sh__)
#define ROMB(o) (*(const uint8_t *const *)((const uint8_t *)snes_pg + (((o) >> 8) & 0x3FFC)))[(o) & 0x3FF]
#else
#define ROMB(o) snes_pg[((o) >> 10) & 0xFFF][(o) & 0x3FF]      /* (host: 8-byte pointers) */
#endif
#endif
#define INSN(pc, m)
#ifdef FCOUNT_PROF
void fcount_hit(uint32_t k);              /* (host test) calls per translated function */
#define FENTER(k) fcount_hit(k)
#else
#define FENTER(k)
#endif
