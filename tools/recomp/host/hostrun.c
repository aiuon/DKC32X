/* Host test runner for the recompiled DKC1 code.
   hostrun rom.sfc ref.wai [nframes] [start]
   ref.wai = per-frame snapshots taken by the instrumented snes9x at the WAI point (8080F7):
   header 12 x u32 (magic, frame, A, X, Y, S, D, DB, P, joy1, joy2, 0), WRAM 128 KB, SRAM 8 KB.
   Starts from snapshot `start`, runs one NMI+frame per snapshot with that snapshot's joypad
   and compares WRAM with the next snapshot. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../r65rt.h"

uint8_t wram[0x20000];
const uint8_t *rom;
uint8_t sram[0x2000];
#ifdef R65_MEMTRACK
unsigned long r65_romreads;
int32_t r65_wh_lastw[0x20000], r65_wh_minw[512], r65_frame_no;
uint8_t r65_rompg[1 << 12], r65_whpg[512], r65_whpg_frame[512], r65_whpg_load[512], r65_whpg_rd[512], r65_whpg_wr[512];
static void whpg_commit(int normal) { for (int i = 0; i < 512; i++) if (r65_whpg_frame[i]) { if (normal) r65_whpg[i] = 1; else r65_whpg_load[i] = 1; r65_whpg_frame[i] = 0; } }
#endif
static uint8_t regs[0x10000];     /* last written I/O register values */
static uint32_t joy1, joy2, wmadd;
static uint32_t apu_port[4];
static uint32_t mul_a, div_a;
static uint32_t m7_a, m7_old;
static long ninsn;
uint32_t apu_port0_echo(uint32_t v);
long r65_icount;
uint32_t *r65_iprof;
static long wd_start;
void r65_watchdog(uint32_t pc) { if (r65_icount - wd_start > 30000000) { fprintf(stderr, "watchdog: interpreter stuck at %06X\n", pc); exit(3); } }
static uint32_t *pccnt, *pccnt_frame; static uint32_t *touched; static int ntouched;
static int verbose;

void r65_fatal(const char *what, uint32_t v) { fprintf(stderr, "fatal %s %x\n", what, v); exit(1); }
static uint32_t *fcnt, *fcnt_frame, *ftouched, *fkey, *ffr; static int nftouched;
void r65_insn(uint32_t pc, uint32_t mode, uint32_t a, uint32_t x, uint32_t y, uint32_t s, uint32_t d, uint32_t db, uint32_t fk)
{
    if (fcnt_frame) { uint32_t i = ((fk >> 24) & 3) << 22 | (fk & 0x3FFFFF); if (!fcnt_frame[i]++) { ftouched[nftouched++] = i; fkey[i] = fk; } }
    ninsn++;
    if (pccnt_frame) { if (!pccnt_frame[pc]++) touched[ntouched++] = pc; }
    if (verbose > 1) fprintf(stderr, "%06X m%d A=%04X X=%04X Y=%04X S=%04X D=%04X DB=%02X\n", pc, mode, a, x, y, s, d, db);
}

static void do_dma(uint32_t mask)
{
    for (int ch = 0; ch < 8; ch++) {
        if (!(mask >> ch & 1)) continue;
        uint8_t *r = &regs[0x4300 + ch * 16];
        uint32_t dmap = r[0], bb = r[1], src = r[2] | r[3] << 8, bank = r[4], n = r[5] | r[6] << 8;
        if (!n) n = 0x10000;
        int step = (dmap & 0x08) ? 0 : (dmap & 0x10) ? -1 : 1;
        static const int pat[8][4] = { {0,0,0,0}, {0,1,0,1}, {0,0,0,0}, {0,0,1,1}, {0,1,2,3}, {0,1,0,1}, {0,0,0,0}, {0,0,1,1} };
        if (verbose) fprintf(stderr, "DMA ch%d dmap %02X B=21%02X src %02X:%04X n %X\n", ch, dmap, bb, bank, src, n);
        for (uint32_t k = 0; k < n; k++) {
            uint32_t b = 0x2100 | ((bb + pat[dmap & 7][k & 3]) & 0xFF);
            if (dmap & 0x80) { /* B -> A */
                mwr8(bank << 16 | src, io_rd(0x800000 | b));
            } else {
                io_wr(0x800000 | b, mrd8(bank << 16 | src));
            }
            src = (src + step) & 0xFFFF;
        }
        r[2] = src; r[3] = src >> 8; r[5] = r[6] = 0;
    }
}

extern uint32_t io_wcnt[0x10000], io_rcnt[0x10000];
uint32_t io_rd(uint32_t ea)
{
    uint32_t a = ea & 0xFFFF;
    io_rcnt[a]++;
    if (a >= 0x6000 && a < 0x8000) { /* SRAM in banks 20-3F / A0-BF */
        uint32_t bank = (ea >> 16) & 0x7F;
        if (bank >= 0x20 && bank < 0x40) return sram[a & 0x7FF];
        return 0;
    }
    switch (a) {
    case 0x2140: return apu_port[0];
    case 0x2141: return 0xFF;
    case 0x2142: return apu_port[2];
    case 0x2143: return apu_port[3];
    case 0x4210: return 0x02;
    case 0x4211: return 0;
    case 0x4212: return 0x80;
    case 0x4214: case 0x4215: case 0x4216: case 0x4217: return regs[a];
    case 0x4218: return joy1 & 0xFF;
    case 0x4219: return joy1 >> 8;
    case 0x421A: return joy2 & 0xFF;
    case 0x421B: return joy2 >> 8;
    case 0x4016: case 0x4017: return 1;
    case 0x2137: return 0;
    case 0x213C: case 0x213D: return 0xE1 & 0xFF; /* V counter: early vblank */
    case 0x213F: return 0;
    case 0x2134: case 0x2135: case 0x2136: {
        int32_t r = (int16_t)m7_a * (int8_t)regs[0x211C];
        return (r >> ((a - 0x2134) * 8)) & 0xFF; }
    case 0x2180: { uint32_t v = wram[wmadd & 0x1FFFF]; wmadd = (wmadd + 1) & 0x1FFFF; return v; }
    case 0x2139: case 0x213A: return 0;
    }
    if (a >= 0x4300 && a < 0x4380) return regs[a];
    if (verbose) fprintf(stderr, "io_rd %06X\n", ea);
    return 0;
}

uint32_t io_wcnt[0x10000], io_rcnt[0x10000];
void io_wr(uint32_t ea, uint32_t v)
{
    uint32_t a = ea & 0xFFFF;
    io_wcnt[a]++;
    v &= 0xFF;
    if (a >= 0x6000 && a < 0x8000) {
        uint32_t bank = (ea >> 16) & 0x7F;
        if (bank >= 0x20 && bank < 0x40) sram[a & 0x7FF] = v;
        return;
    }
    regs[a] = v;
    switch (a) {
    case 0x2140: case 0x2141: case 0x2142: case 0x2143: apu_port[a & 3] = v;
        if (getenv("APULOG") && a == 0x2140) fprintf(stderr, "apu %02X %02X %02X %02X\n", apu_port[0], apu_port[1], apu_port[2], apu_port[3]);
        break;
    case 0x2180: wram[wmadd & 0x1FFFF] = v; wmadd = (wmadd + 1) & 0x1FFFF; break;
    case 0x2181: wmadd = (wmadd & 0x1FF00) | v; break;
    case 0x2182: wmadd = (wmadd & 0x100FF) | v << 8; break;
    case 0x2183: wmadd = (wmadd & 0x0FFFF) | (v & 1) << 16; break;
    case 0x211B: m7_a = (m7_a >> 8) | v << 8; break;
    case 0x4203: { uint32_t r = regs[0x4202] * v; regs[0x4216] = r; regs[0x4217] = r >> 8; break; }
    case 0x4206: {
        uint32_t dvd = regs[0x4204] | regs[0x4205] << 8, q, r;
        if (v) { q = dvd / v; r = dvd % v; } else { q = 0xFFFF; r = dvd; }
        regs[0x4214] = q; regs[0x4215] = q >> 8; regs[0x4216] = r; regs[0x4217] = r >> 8; break; }
    case 0x420B: do_dma(v); break;
    }
}

uint32_t apu_port0_echo(uint32_t v) { apu_port[0] = v; return v; }

typedef struct { uint32_t h[12]; uint8_t ram[0x20000]; uint8_t sram[0x2000]; } snap_t;

static int read_snap(FILE *f, long idx, snap_t *s)
{
    if (fseek(f, idx * (long)sizeof(snap_t), SEEK_SET)) return 0;
    return fread(s, sizeof *s, 1, f) == 1;
}

static long nnorm;
static void cnt_init(const char *cntf)
{
    if (!cntf) return;
    pccnt = calloc(1 << 24, 4); pccnt_frame = calloc(1 << 24, 4); touched = calloc(1 << 20, 4);
    fcnt = calloc(1 << 24, 4); fcnt_frame = calloc(1 << 24, 4); ftouched = calloc(1 << 20, 4); fkey = calloc(1 << 24, 4); ffr = calloc(1 << 24, 4);
}
static void cnt_frame(int normal)
{
    if (!pccnt) return;
    for (int i = 0; i < ntouched; i++) { if (normal) pccnt[touched[i]] += pccnt_frame[touched[i]]; pccnt_frame[touched[i]] = 0; }
    ntouched = 0; nnorm += normal;
    for (int i = 0; i < nftouched; i++) { if (normal) { fcnt[ftouched[i]] += fcnt_frame[ftouched[i]]; ffr[ftouched[i]]++; } fcnt_frame[ftouched[i]] = 0; }
    nftouched = 0;
}
static void cnt_write(const char *cntf)
{
    if (fcnt) { char fn[512]; snprintf(fn, sizeof fn, "%s.func", cntf); FILE *o = fopen(fn, "a"); fprintf(o, "# %ld\n", nnorm);
        for (int i = 0; i < (1 << 24); i++) if (fcnt[i]) fprintf(o, "%08X %u %u\n", fkey[i], fcnt[i], ffr[i]);
        fclose(o); }
    if (pccnt) { FILE *o = fopen(cntf, "a"); fprintf(o, "# %ld\n", nnorm); for (int i = 0; i < (1 << 24); i++) if (pccnt[i]) fprintf(o, "%06x %u\n", i, pccnt[i]); fclose(o); }
}
int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: hostrun rom ref.wai [nframes] [start]\n"); return 1; }
    FILE *rf = fopen(argv[1], "rb"); static uint8_t romb[0x400000]; fread(romb, 1, sizeof romb, rf); fclose(rf); rom = romb;
    FILE *wf = fopen(argv[2], "rb");
    int nfr = argc > 3 ? atoi(argv[3]) : 100000; long start = argc > 4 ? atol(argv[4]) : 1;
    verbose = getenv("V") ? atoi(getenv("V")) : 0;
    static snap_t s0, s1;
    if (getenv("IPROF")) r65_iprof = calloc(1 << 24, 4);
    cnt_init(getenv("PCCOUNT"));
    if (getenv("FREERUN")) {
        /* free-running: start from snapshot `start`, scripted pad (the 32X GAMEDEMO), no reference */
        if (!read_snap(wf, start, &s0)) return 1;
        memcpy(wram, s0.ram, sizeof wram); memcpy(sram, s0.sram, sizeof sram);
        cpu_t cpu = {0};
        cpu.a = s0.h[2]; cpu.x = s0.h[3]; cpu.y = s0.h[4]; cpu.s = s0.h[5]; cpu.d = s0.h[6]; cpu.db = s0.h[7]; cpu.p = s0.h[8];
        cpu.pc = ((s0.h[8] & 0x30) << 20) | (s0.h[11] & 0xFF0000) | ((s0.h[11] + 1) & 0xFFFF);
        apu_port[0] = wram[0];
        for (int f = 1; f <= nfr; f++) {
            uint32_t sp = 0x0100 | 0x4000;
            if ((f % 90) < 12) sp |= 0x8000;
            if ((f % 600) > 560) sp = 0x0200;
            if (getenv("PAUSEAT")) { int pf = atoi(getenv("PAUSEAT")); if (f >= pf && f < pf + 120) sp = (f == pf || f == pf + 60) ? 0x1000 : 0; }
            joy1 = sp; joy2 = 0;
            uint32_t s = cpu.s, pc = cpu.pc;
            wram[s] = (pc >> 16) & 0xFF; s = (s - 1) & 0xFFFF; wram[s] = pc >> 8; s = (s - 1) & 0xFFFF;
            wram[s] = pc; s = (s - 1) & 0xFFFF; wram[s] = cpu.p; s = (s - 1) & 0xFFFF;
            cpu.s = s; cpu.p = (cpu.p | 0x04) & ~0x08;
            ninsn = 0; wd_start = r65_icount; long ic0 = r65_icount;
            r65_frame_no = f;
            if (f == 1) { for (int i = 0; i < 0x20000; i++) r65_wh_lastw[i] = -1; for (int i = 0; i < 512; i++) r65_wh_minw[i] = 0x7FFFFFFF; }
            run65(&cpu, ((cpu.p & 0x30) << 20) | 0x00A968);
            if (cpu.err) { fprintf(stderr, "frame %d: error %08X\n", f, cpu.errpc); return 1; }
            cnt_frame((cpu.pc & 0xFFFFFF) == 0x8080F8 && ninsn + (r65_icount - ic0) < 40000);
#ifdef R65_MEMTRACK
            if (getenv("WHSTAT")) {
                /* high WRAM page classes: N accessed in gameplay frames; P accessed in a load sequence
                   while holding data from before it; T only within the sequence that wrote it */
                static uint8_t prev[0x20000], cls[512]; static int seqstart, inload, init;
                int normal = (cpu.pc & 0xFFFFFF) == 0x8080F8 && ninsn + (r65_icount - ic0) < 40000;
                if (!init) { memcpy(prev, s0.ram, sizeof prev); init = 1; }
                if (!normal && !inload) seqstart = f;
                inload = !normal;
                for (int pg = 0x20; pg < 512; pg++) {
                    int ch = memcmp(prev + (pg << 8), wram + (pg << 8), 256) != 0;
                    int rd = r65_whpg_rd[pg], wr = r65_whpg_wr[pg] || ch;
                    if (normal) { if (rd || wr) cls[pg] |= 1; }
                    else {
                        if (rd && r65_wh_minw[pg] < seqstart) cls[pg] |= 2;     /* read data written before this load */
                        if (rd || wr) cls[pg] |= 8;
                    }
                    if (ch) for (int i = 0; i < 256; i++) if (prev[(pg << 8) + i] != wram[(pg << 8) + i] && r65_wh_lastw[(pg << 8) + i] != f) r65_wh_lastw[(pg << 8) + i] = f;   /* DMA writes */
                    r65_whpg_rd[pg] = r65_whpg_wr[pg] = 0; r65_wh_minw[pg] = 0x7FFFFFFF;
                }
                memcpy(prev, wram, sizeof prev);
                if (getenv("FRAMELOG")) {
                    static uint8_t tr[512]; static int trl;
                    if (!trl) { FILE *c = fopen(getenv("FRAMELOG"), "r"); unsigned pg, v;
                        while (c && fscanf(c, "%x %u", &pg, &v) == 2) tr[pg] = !(v & 3); if (c) fclose(c); trl = 1; }
                    int t = 0; for (int pg = 0; pg < 512; pg++) if (tr[pg] && r65_whpg_frame[pg]) t++;
                    fprintf(stdout, "F %d %06X %02X %d\n", f, cpu.pc & 0xFFFFFF, regs[0x2100], t);
                }
                if (f == nfr) { FILE *o = fopen(getenv("WHSTAT"), "a");
                    for (int pg = 0x20; pg < 512; pg++) if (cls[pg]) fprintf(o, "%03X %d\n", pg, cls[pg]);
                    fclose(o); }
            }
            whpg_commit((cpu.pc & 0xFFFFFF) == 0x8080F8 && ninsn + (r65_icount - ic0) < 40000);
#endif
            if (r65_iprof && atoi(getenv("IPROF")) >= 0 && f == atoi(getenv("IPROF"))) {
                FILE *o = fopen("/tmp/iprof.txt", "w");
                for (int i = 0; i < (1 << 24); i++) if (r65_iprof[i]) fprintf(o, "%06X %u\n", i, r65_iprof[i]);
                fclose(o); return 0; }
            if (r65_iprof && atoi(getenv("IPROF")) >= 0) memset(r65_iprof, 0, 4 << 24);
            { extern long r65_nxcall, r65_nxinterp; static long lx, li;
            if (verbose || f % 50 == 0 || (cpu.pc & 0xFFFFFF) != 0x8080F8)
                fprintf(stderr, "frame %d: resume %08X camx %d insns %ld+%ld xcalls %ld interp-entries %ld\n", f, cpu.pc, wram[0x88B] | wram[0x88C] << 8, ninsn, r65_icount - ic0, r65_nxcall - lx, r65_nxinterp - li);
            lx = r65_nxcall; li = r65_nxinterp; }
        }
        { extern uint32_t r65_ikey[], r65_ikeyn[]; extern int r65_nikey;
          FILE *o = fopen("/tmp/ikeys.txt", "w");
          for (int i = 0; i < r65_nikey; i++) fprintf(o, "%08X %u\n", r65_ikey[i], r65_ikeyn[i]);
          fclose(o); }
        if (getenv("PCCOUNT")) cnt_write(getenv("PCCOUNT"));
        fprintf(stderr, "ROM reads %lu (%.1f per frame)\n", r65_romreads, (double)r65_romreads / nfr);
        if (getenv("BANKCOUNT")) { extern uint32_t r65_mrd16_bank[256], r65_mrd8_bank[256];
            for (int i = 0; i < 256; i++) if (r65_mrd16_bank[i] || r65_mrd8_bank[i]) fprintf(stderr, "bank %02X rd16 %u rd8 %u\n", i, r65_mrd16_bank[i], r65_mrd8_bank[i]);
            extern uint32_t r65_mrd8_b0[256]; for (int i = 0; i < 256; i++) if (r65_mrd8_b0[i] > 1000) fprintf(stderr, "b0 page %02X00 %u\n", i, r65_mrd8_b0[i]); }
        if (getenv("IOCOUNT")) { FILE *o = fopen(getenv("IOCOUNT"), "w");
            for (int i = 0; i < 0x10000; i++) if (io_wcnt[i] || io_rcnt[i]) fprintf(o, "%04X w %u r %u\n", i, io_wcnt[i], io_rcnt[i]);
            fclose(o); }
        if (r65_iprof) { FILE *o = fopen("/tmp/iprof.txt", "a");
            for (int i = 0; i < (1 << 24); i++) if (r65_iprof[i]) fprintf(o, "%06X %u\n", i, r65_iprof[i]);
            fclose(o); }
#ifdef R65_MEMTRACK
        { const char *mf = getenv("MEMTRACK"); if (mf) { FILE *o = fopen(mf, "a");
            for (int i = 0; i < (1 << 12); i++) if (r65_rompg[i]) fprintf(o, "R %03X\n", i);
            for (int i = 0; i < 512; i++) if (r65_whpg[i]) fprintf(o, "W %03X\n", i);
            for (int i = 0; i < 512; i++) if (r65_whpg_load[i]) fprintf(o, "L %03X\n", i);
            fclose(o); } }
#endif
        return 0;
    }
    if (!read_snap(wf, start, &s0)) { fprintf(stderr, "no start snapshot\n"); return 1; }
    memcpy(wram, s0.ram, sizeof wram); memcpy(sram, s0.sram, sizeof sram);
    cpu_t cpu = {0};
    cpu.a = s0.h[2]; cpu.x = s0.h[3]; cpu.y = s0.h[4]; cpu.s = s0.h[5]; cpu.d = s0.h[6]; cpu.db = s0.h[7]; cpu.p = s0.h[8];
    cpu.pc = 0x8080F8;
    apu_port[0] = wram[0];   /* the sound driver toggles $00 and waits for the echo */
    long total = 0;
    const char *cntf = getenv("PCCOUNT");
    for (int f = 0; f < nfr; f++) {
        if (!read_snap(wf, start + f + 1, &s1)) break;
        joy1 = s1.h[9]; joy2 = s1.h[10];
        /* NMI: push PB, PC, P; I = 1, D = 0 */
        uint32_t s = cpu.s, pc = cpu.pc;
        wram[s] = (pc >> 16) & 0xFF; s = (s - 1) & 0xFFFF; wram[s] = pc >> 8; s = (s - 1) & 0xFFFF;
        wram[s] = pc; s = (s - 1) & 0xFFFF; wram[s] = cpu.p; s = (s - 1) & 0xFFFF;
        cpu.s = s; cpu.p = (cpu.p | 0x04) & ~0x08;
        ninsn = 0; wd_start = r65_icount;
        run65(&cpu, ((cpu.p & 0x30) << 20) | 0x00A968);
        total += ninsn;
        if (pccnt) {
            int normal = (cpu.pc & 0xFFFFFF) == 0x8080F8 && ninsn < 40000;
            static long tsum, tn, tmax; if (normal) { tsum += ntouched; tn++; if (ntouched > tmax) tmax = ntouched; }
            if (f == nfr - 1 || (normal && tn % 200 == 0)) fprintf(stderr, "unique pcs/frame avg %ld max %ld\n", tn ? tsum / tn : 0, tmax);
            cnt_frame(normal);
        }
        if (cpu.err) { fprintf(stderr, "frame %d: error key %08X\n", f, cpu.errpc); return 1; }
        int nd = 0, first = -1;
        for (int i = 0; i < 0x20000; i++) {
            if (i >= 0x100 && i < 0x200) continue;   /* stack page: lag-frame pushes */
            if (wram[i] != s1.ram[i]) { if (first < 0) first = i; nd++; }
        }
        if (nd || verbose) fprintf(stderr, "frame %d: our resume %08X, snes WAI at %06X, %ld insns\n", f, cpu.pc, s1.h[11], ninsn);
        if (nd) {
            fprintf(stderr, "frame %d (snes %u): %d WRAM bytes differ, first %05X (ours %02X snes %02X)\n", f, s1.h[1], nd, first, wram[first], s1.ram[first]);
            int k = 0;
            for (int i = 0; i < 0x20000 && k < 40; i++) {
                if (i >= 0x100 && i < 0x200) continue;
                if (wram[i] != s1.ram[i]) { fprintf(stderr, "  %05X ours %02X snes %02X (was %02X)\n", i, wram[i], s1.ram[i], s0.ram[i]); k++; }
            }
            if (!getenv("CONT")) return 2;
            memcpy(wram, s1.ram, sizeof wram); memcpy(sram, s1.sram, sizeof sram);
            cpu.a = s1.h[2]; cpu.x = s1.h[3]; cpu.y = s1.h[4]; cpu.s = s1.h[5]; cpu.d = s1.h[6]; cpu.db = s1.h[7]; cpu.p = s1.h[8];
            cpu.pc = ((s1.h[8] & 0x30) << 20) | (s1.h[11] & 0xFF0000) | ((s1.h[11] + 1) & 0xFFFF);
            apu_port[0] = wram[0];
        }
        if (cpu.a != s1.h[2] || cpu.x != s1.h[3] || cpu.y != s1.h[4] || cpu.s != s1.h[5])
            fprintf(stderr, "frame %d: regs differ A %04X/%04X X %04X/%04X Y %04X/%04X S %04X/%04X\n", f,
                    cpu.a, s1.h[2], cpu.x, s1.h[3], cpu.y, s1.h[4], cpu.s, s1.h[5]);
        s0 = s1;
    }
    fprintf(stderr, "ok, avg %ld insns/frame, interpreted %ld total\n", total / (nfr ? nfr : 1), r65_icount);
#ifdef R65_MEMTRACK
    { const char *mf = getenv("MEMTRACK"); if (mf) { FILE *o = fopen(mf, "a");
        for (int i = 0; i < (1 << 12); i++) if (r65_rompg[i]) fprintf(o, "R %03X\n", i);
        for (int i = 0; i < 512; i++) if (r65_whpg[i]) fprintf(o, "W %03X\n", i);
        for (int i = 0; i < 512; i++) if (r65_whpg_load[i]) fprintf(o, "L %03X\n", i);
        fclose(o); } }
#endif
    if (cntf) cnt_write(cntf);
    return 0;
}
