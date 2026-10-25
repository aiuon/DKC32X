/* r65 runtime helpers shared by the host test build and the 32X build */
#define R65_REGS
#include "r65rt.h"

#if !defined(R65_SH2)
uint32_t r65_a, r65_x, r65_y, r65_s, r65_z, r65_c;
#endif
int32_t r65_n;
uint32_t r65_d, r65_db, r65_mx, r65_v, r65_fd, r65_fi;

#ifdef R65_HOST
long r65_nxcall, r65_nxinterp;
#endif
#ifdef R65_STACKTRACK
char *r65_stk_min = (char *)-1; int r65_depth, r65_maxdepth;
#endif
volatile uint32_t r65_dbg_key, r65_dbg_n;
/* (I13) dispatch cache: 512 direct-mapped entries, key and function side by side (one cache line fill
   per lookup instead of two), multiplicative hash (the xor fold left ~8 % of the lookups colliding:
   a binary search over the ROM key table each) */
#define XC_N 512
static struct { uint32_t key; uint32_t (*fn)(void); } xc_t[XC_N] __attribute__((aligned(16)));
#define XC_HASH(k) ((((k) * 0x9E3779B1u) >> 23) & (XC_N - 1))
static uint32_t (*xlookup(uint32_t key))(void)
{
    int lo = 0, hi = r65_ndkeys - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1; uint32_t k = r65_dkeys[mid];
        if (k == key) return r65_dfuncs[mid];
        if (k < key) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}
#ifdef XC_PROF
#include <stdio.h>
#include <stdlib.h>
static uint32_t xcp_k[4096], xcp_n[4096], xcp_i[4096], xcp_g[4096]; static int xcp_c;
extern uint32_t r65_ibudget;
static uint32_t (*xlookup(uint32_t key))(void);
__attribute__((destructor)) static void xcp_dump(void) { FILE *f = fopen(getenv("XCFILE") ? getenv("XCFILE") : "/tmp/xc.txt", "w"); for (int i = 0; i < xcp_c; i++) fprintf(f, "%08X %u %d %u %u\n", xcp_k[i], xcp_n[i], xlookup(xcp_k[i]) != 0, xcp_i[i], xcp_g[i]); fclose(f); }
#endif
#ifdef IKEYPROF
uint32_t ikp_key[48], ikp_n[48], ikp_c[48];
#endif
/* (test builds) interpreted calls: instructions per key - IKEYPROF (32X: ikp_* read from an SDRAM dump),
   XC_PROF (host: the xc file's 4th column) */
#if defined(IKEYPROF) || defined(XC_PROF)
static uint32_t interp_c(uint32_t key)
{
    extern uint32_t r65_ibudget;
    /* (I16) exclusive counts: the instructions of interpreted calls made from inside are the callees' */
    static uint32_t child;
    uint32_t sv = child; child = 0;
    uint32_t b0 = r65_ibudget, r = interp(key), d = r65_ibudget - b0, dd = d;
    d -= child; child = sv + dd;
#ifdef IKEYPROF
    for (int i = 0; i < 48; i++)
        if (ikp_key[i] == key || !ikp_key[i]) { ikp_key[i] = key; ikp_n[i] += d; ikp_c[i]++; break; }
#else
    { extern uint8_t game_fblank;              /* (I16) 5th column: in frames with a picture */
      for (int i = 0; i < xcp_c; i++) if (xcp_k[i] == key) { xcp_i[i] += d; if (!game_fblank) xcp_g[i] += d; break; } }
#endif
    return r;
}
#define INTERP_C(k) interp_c(k)
#else
#define INTERP_C(k) interp(k)
#endif
SDRAM_FN uint32_t xcall(uint32_t key)
{
#ifdef XC_PROF
    { int i; for (i = 0; i < xcp_c && xcp_k[i] != key; i++); if (i == xcp_c && xcp_c < 4096) { xcp_k[xcp_c] = key; xcp_n[xcp_c++] = 0; } if (i < 4096) xcp_n[i]++; }
#endif
    uint32_t h = XC_HASH(key);
    r65_dbg_key = key;
#ifdef R65_FORCEI
    { extern int r65_forcei; if (r65_forcei) return INTERP_C(key); }
#endif
#ifdef R65_HOST
    r65_nxcall++;
#endif
    if (xc_t[h].key == key + 1) {
        uint32_t (*f)(void) = xc_t[h].fn;
#ifdef R65_HOST
        if (!f) { void note_ikey_(uint32_t); note_ikey_(key); }
#endif
        return f ? f() : INTERP_C(key);
    }
    {
        uint32_t (*f)(void) = xlookup(key);
        xc_t[h].key = key + 1; xc_t[h].fn = f;
        if (f) return f();
#ifdef R65_HOST
        r65_nxinterp++;
#endif
#ifdef R65_HOST
        { void note_ikey_(uint32_t); note_ikey_(key); }
#endif
        return INTERP_C(key);
    }
}
#ifdef R65_HOST
uint32_t r65_ikey[4096], r65_ikeyn[4096]; int r65_nikey;
static void note_ikey(uint32_t key)
{
    for (int i = 0; i < r65_nikey; i++) if (r65_ikey[i] == key) { r65_ikeyn[i]++; return; }
    if (r65_nikey < 4096) { r65_ikey[r65_nikey] = key; r65_ikeyn[r65_nikey++] = 1; }
}
void note_ikey_(uint32_t key) { note_ikey(key); }
#endif
uint32_t xcall_old(uint32_t key)
{
    r65_dbg_key = key; r65_dbg_n++;
#ifdef R65_HOST
    r65_nxcall++;
#endif
#ifdef R65_STACKTRACK
    { char c; if (&c < r65_stk_min) r65_stk_min = &c; }
#endif
    int lo = 0, hi = r65_ndkeys - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1; uint32_t k = r65_dkeys[mid];
        if (k == key) return r65_dfuncs[mid]();
        if (k < key) lo = mid + 1; else hi = mid - 1;
    }
#ifdef R65_HOST
    r65_nxinterp++;
#endif
    return interp(key);
}

/* run a call to key: follow tail transfers (JUMPKEY results) without growing the C stack */
SDRAM_FN uint32_t xrun(uint32_t key)
{
    uint32_t r = xcall(key & ~JUMPKEY);
    while (r & JUMPKEY) r = xcall(r & ~JUMPKEY);
    return r;
}

SDRAM_FN uint32_t o_rd8_b0(uint32_t a) { return rd8_b0(a); }
SDRAM_FN uint32_t o_rd16_b0(uint32_t a) { return rd16_b0(a); }
SDRAM_FN void o_wr8_b0(uint32_t a, uint32_t v) { wr8_b0(a, v); }
SDRAM_FN void o_wr16_b0(uint32_t a, uint32_t v) { wr16_b0(a, v); }
SDRAM_FN uint32_t o_rd8_sys(uint32_t db, uint32_t off) { return rd8_sys(db, off); }
SDRAM_FN uint32_t o_rd16_sys(uint32_t db, uint32_t off) { return rd16_sys(db, off); }
SDRAM_FN uint32_t o_rd8_sysr(uint32_t db, uint32_t off) { return rd8_sysr(db, off); }
SDRAM_FN uint32_t o_rd16_sysr(uint32_t db, uint32_t off) { return rd16_sysr(db, off); }
SDRAM_FN void o_wr8_sys(uint32_t db, uint32_t off, uint32_t v) { wr8_sys(db, off, v); }
SDRAM_FN void o_wr16_sys(uint32_t db, uint32_t off, uint32_t v) { wr16_sys(db, off, v); }

/* calls and returns (generated code: r65.py call() / RTS / RTL) */
#define LPUSH(v) do { CHECK_S_(); WL(r65_s) = (uint8_t)(v); r65_s = r65_s - 1; } while (0)
#ifdef R65_HOST
#define CHECK_S_() do { if (r65_s >= 0x2000) r65_fatal("stack", r65_s); } while (0)
#else
#define CHECK_S_()
#endif
SDRAM_FN uint32_t r65_call2(uint32_t (*f)(void), uint32_t exp)
{
    uint32_t ra = exp - 1;
    LPUSH(ra >> 8); LPUSH(ra);
    uint32_t r = f();
    if (r & JUMPKEY) r = xrun(r);
    return r == exp ? 0 : r;
}
SDRAM_FN uint32_t r65_call3(uint32_t (*f)(void), uint32_t exp)
{
    uint32_t ra = exp - 1;
    LPUSH(exp >> 16); LPUSH(ra >> 8); LPUSH(ra);
    uint32_t r = f();
    if (r & JUMPKEY) r = xrun(r);
    return r == exp ? 0 : r;
}
SDRAM_FN uint32_t r65_callx2(uint32_t key, uint32_t exp)
{
    uint32_t ra = exp - 1;
    LPUSH(ra >> 8); LPUSH(ra);
    uint32_t r = xrun(key);
    return r == exp ? 0 : r;
}
SDRAM_FN uint32_t r65_callx3(uint32_t key, uint32_t exp)
{
    uint32_t ra = exp - 1;
    LPUSH(exp >> 16); LPUSH(ra >> 8); LPUSH(ra);
    uint32_t r = xrun(key);
    return r == exp ? 0 : r;
}
SDRAM_FN uint32_t r65_rts(uint32_t base)
{
    uint32_t s1 = r65_s + 1, l = WL(s1), h = WL(s1 + 1);
    r65_s = s1 + 1;
    return base | ((l | h << 8) + 1) & 0xFFFF;
}
SDRAM_FN uint32_t r65_rtl(uint32_t mode)
{
    uint32_t s1 = r65_s + 1, l = WL(s1), h = WL(s1 + 1), b = WL(s1 + 2);
    r65_s = s1 + 2;
    return mode | b << 16 | ((l | h << 8) + 1) & 0xFFFF;
}

/* run from key until WAI (cpu->pc = resume key) or an error (cpu->err) */
SDRAM_FN void run65(cpu_t *cpu, uint32_t key)
{
    uint32_t p = cpu->p;
    r65_a = cpu->a; r65_x = cpu->x; r65_y = cpu->y; r65_s = cpu->s; r65_d = cpu->d; r65_db = cpu->db;
    r65_mx = p & 0x30; r65_n = (p & 0x80) ? -1 : 0; r65_v = (p >> 6) & 1; r65_fd = (p >> 3) & 1;
    r65_fi = (p >> 2) & 1; r65_z = (p & 2) ? 0 : 1; r65_c = p & 1;
    cpu->err = 0;
    for (;;) {
        uint32_t r = xrun(key);
        if (r & WAITKEY) { cpu->pc = r & ~WAITKEY; r65_mx = (r >> 20) & 0x30; break; }
        if (r & ERRKEY) { cpu->err = 1; cpu->errpc = r; break; }
        key = r;
    }
    cpu->a = r65_a; cpu->x = r65_x; cpu->y = r65_y; cpu->s = r65_s; cpu->d = r65_d; cpu->db = r65_db;
    cpu->p = ((r65_n < 0) << 7) | (r65_v << 6) | r65_mx | (r65_fd << 3) | (r65_fi << 2) | ((r65_z == 0) << 1) | r65_c;
}

SDRAM_FN uint32_t mrd8(uint32_t ea)
{
#ifdef R65_HOST
    { extern uint32_t r65_mrd8_bank[256], r65_mrd8_b0[256]; r65_mrd8_bank[ea >> 16]++; if (!(ea >> 16)) r65_mrd8_b0[(ea >> 8) & 255]++; }
#endif
    return mrd8_i(ea);
}
void mwr8(uint32_t ea, uint32_t v) { mwr8_i(ea, v); }
#ifdef R65_HOST
uint32_t r65_mrd16_bank[256], r65_mrd8_bank[256], r65_mrd8_b0[256];
#endif
#ifdef MRD_PROF
#include <stdio.h>
static void *mrd_ra[4096]; static uint32_t mrd_rn[4096]; static int mrd_n;
static void mrd_note(void *ra) { for (int i = 0; i < mrd_n; i++) if (mrd_ra[i] == ra) { mrd_rn[i]++; return; } if (mrd_n < 4096) { mrd_ra[mrd_n] = ra; mrd_rn[mrd_n++] = 1; } }
__attribute__((destructor)) static void mrd_dump(void) { FILE *f = fopen("/tmp/mrd.txt", "w"); for (int i = 0; i < mrd_n; i++) fprintf(f, "%p %u\n", mrd_ra[i], mrd_rn[i]); fclose(f); }
#endif
SDRAM_FN uint32_t mrd16(uint32_t ea)
{
#ifdef MRD_PROF
    mrd_note(__builtin_return_address(0));
#endif
#ifdef R65_HOST
    r65_mrd16_bank[ea >> 16]++;
#endif
    uint32_t bank = ea >> 16, off = ea & 0xFFFF;
    if ((off & 0x3FF) != 0x3FF) {
        if (!(bank & 0x40)) {
            if (off < 0x1FFF) return WL(off) | WL(off + 1) << 8;
            if (off >= 0x8000) { const uint8_t *p = &ROMB(((bank & 0x3F) << 16) | off); return p[0] | p[1] << 8; }
        } else if ((bank & 0xFE) == 0x7E) {
            const uint8_t *p = &WH(ea & 0x1FFFF); return p[0] | p[1] << 8;
        } else {
            const uint8_t *p = &ROMB(ea & 0x3FFFFF); return p[0] | p[1] << 8;
        }
    }
    return mrd8_i(ea) | mrd8_i((ea + 1) & 0xFFFFFF) << 8;
}
SDRAM_FN uint32_t mrd8s(uint32_t db, uint32_t off) { return mrd8(((db << 16) + off) & 0xFFFFFF); }
#ifdef MRD_PROF
static uint32_t mrds_h[0x10000];
__attribute__((destructor)) static void mrds_dump(void) { FILE *f = fopen("/tmp/mrds.txt", "w"); for (int i = 0; i < 0x10000; i++) if (mrds_h[i]) fprintf(f, "%04X %u\n", i, mrds_h[i]); fclose(f); }
#endif
SDRAM_FN uint32_t mrd16s(uint32_t db, uint32_t off)
{
#ifdef MRD_PROF
    mrds_h[(db & 0xFF) << 8 | ((off >> 8) & 0xFF)]++;
#endif
    return mrd16(((db << 16) + off) & 0xFFFFFF);
}
void mwr8s(uint32_t db, uint32_t off, uint32_t v) { mwr8_i(((db << 16) + off) & 0xFFFFFF, v); }
void mwr16s(uint32_t db, uint32_t off, uint32_t v) { mwr16(((db << 16) + off) & 0xFFFFFF, v); }
void mwr16(uint32_t ea, uint32_t v) { mwr8_i(ea, v); mwr8_i((ea + 1) & 0xFFFFFF, v >> 8); }
uint32_t rd16_slow(uint32_t a) { return mrd8_i(a) | mrd8_i((a + 1) & 0xFFFF) << 8; }

uint32_t dec_adc16(uint32_t A, uint32_t W, uint32_t carry)
{
    uint32_t r, v;
    A &= 0xFFFF; W &= 0xFFFF;
    r = (A & 0x000F) + (W & 0x000F) + carry; if (r > 0x0009) r += 0x0006; carry = r > 0x000F;
    r = (A & 0x00F0) + (W & 0x00F0) + (r & 0x000F) + carry * 0x10; if (r > 0x009F) r += 0x0060; carry = r > 0x00FF;
    r = (A & 0x0F00) + (W & 0x0F00) + (r & 0x00FF) + carry * 0x100; if (r > 0x09FF) r += 0x0600; carry = r > 0x0FFF;
    r = (A & 0xF000) + (W & 0xF000) + (r & 0x0FFF) + carry * 0x1000;
    v = (A & 0x8000) == (W & 0x8000) && (A & 0x8000) != (r & 0x8000);
    if (r > 0x9FFF) r += 0x6000;
    return (r & 0xFFFF) | (r > 0xFFFF) << 16 | v << 17;
}
uint32_t dec_adc8(uint32_t A, uint32_t W, uint32_t carry)
{
    uint32_t r, v;
    A &= 0xFF; W &= 0xFF;
    r = (A & 0x0F) + (W & 0x0F) + carry; if (r > 0x09) r += 0x06; carry = r > 0x0F;
    r = (A & 0xF0) + (W & 0xF0) + (r & 0x0F) + carry * 0x10;
    v = (A & 0x80) == (W & 0x80) && (A & 0x80) != (r & 0x80);
    if (r > 0x9F) r += 0x60;
    return (r & 0xFF) | (r > 0xFF) << 8 | v << 9;
}
uint32_t dec_sbc16(uint32_t A, uint32_t W, uint32_t carry)
{
    int r; uint32_t v;
    A &= 0xFFFF; W = (W & 0xFFFF) ^ 0xFFFF;
    r = (A & 0x000F) + (W & 0x000F) + carry; if (r < 0x0010) r -= 0x0006; carry = r > 0x000F;
    r = (A & 0x00F0) + (W & 0x00F0) + (r & 0x000F) + carry * 0x10; if (r < 0x0100) r -= 0x0060; carry = r > 0x00FF;
    r = (A & 0x0F00) + (W & 0x0F00) + (r & 0x00FF) + carry * 0x100; if (r < 0x1000) r -= 0x0600; carry = r > 0x0FFF;
    r = (A & 0xF000) + (W & 0xF000) + (r & 0x0FFF) + carry * 0x1000;
    v = ((A ^ W) & 0x8000) == 0 && ((A ^ (uint32_t)r) & 0x8000);
    if (r < 0x10000) r -= 0x6000;
    return ((uint32_t)r & 0xFFFF) | (r > 0xFFFF) << 16 | v << 17;
}
uint32_t dec_sbc8(uint32_t A, uint32_t W, uint32_t carry)
{
    int r; uint32_t v;
    A &= 0xFF; W = (W & 0xFF) ^ 0xFF;
    r = (A & 0x0F) + (W & 0x0F) + carry; if (r < 0x10) r -= 0x06; carry = r > 0x0F;
    r = (A & 0xF0) + (W & 0xF0) + (r & 0x0F) + carry * 0x10;
    v = ((A ^ W) & 0x80) == 0 && ((A ^ (uint32_t)r) & 0x80);
    if (r < 0x100) r -= 0x60;
    return ((uint32_t)r & 0xFF) | (r > 0xFF) << 8 | v << 9;
}
void blockmove(uint32_t src, uint32_t dst, int dir)
{
    uint32_t a = r65_a & 0xFFFF, x = r65_x, y = r65_y;
    for (;;) {
        mwr8(dst << 16 | y, mrd8(src << 16 | x));
        x = (x + dir) & 0xFFFF; y = (y + dir) & 0xFFFF;
        if (a-- == 0) break;
    }
    r65_a = 0xFFFF; r65_x = x; r65_y = y;
}
