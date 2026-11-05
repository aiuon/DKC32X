/* (I25, devkit) SOUND MD Z80 player harness: z80/mddrv.bin on cz80 with the YM2612 timer A model; this
   program plays the 68000: it keeps the ring ~300 entries ahead (a saw as samples, a register write in
   every K-th entry), holds the bus now and then (H bytes every vblank, in pieces of 16), and checks that
   every timer period plays exactly one entry: missed periods, the longest delay overflow -> DAC write.
   mdharness mddrv.bin units seconds K [holdbytes] */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "cz80.h"
static uint8_t ram[0x2000];
static long long zcyc; static double ta_acc, tb_acc; static int ta, tb, mode, status; static long ovf, missed, dacn, writes, psgn, bad;
static long long lastovf, maxdelay; static int pend_ovf;
static cz80_struc z; static int ym_addr[2]; static double ycpz = 7670453.0 / 3579545.0;
static void timer_run(void)
{
    static long long last; double y = (zcyc - last) * ycpz; last = zcyc;
    if (!(mode & 1)) return;
    ta_acc += y; double per = (1024 - ta) * 144.0;
    while (ta_acc >= per) { ta_acc -= per; ovf++; if (mode & 4) { if (status & 1) missed++; status |= 1; lastovf = zcyc - (long long)(ta_acc / ycpz); pend_ovf = 1; } }
    if (mode & 2) { tb_acc += y; double pb = (256 - tb) * 144.0 * 16; while (tb_acc >= pb) { tb_acc -= pb; if (mode & 8) status |= 2; } }
}
static UINT8 rd(UINT32 a) { a &= 0xFFFF; if (a < 0x4000) return ram[a & 0x1FFF]; if (a < 0x6000) { timer_run(); return (uint8_t)status; } return 0xFF; }
static uint8_t expect_reg = 0; static uint32_t w68;
static void wr(UINT32 a, UINT8 d)
{
    a &= 0xFFFF;
    if (a < 0x4000) { ram[a & 0x1FFF] = d; return; }
    if (a < 0x6000) {
        int port = a & 3, part = port >> 1;
        if (!(port & 1)) { ym_addr[part] = d; return; }
        int r = ym_addr[part];
        if (part == 0 && r == 0x24) ta = (ta & 3) | d << 2;
        else if (part == 0 && r == 0x25) ta = (ta & 0x3FC) | (d & 3);
        else if (part == 0 && r == 0x26) tb = d;
        else if (part == 0 && r == 0x27) { timer_run(); if ((d & 1) && !(mode & 1)) ta_acc = 0; if ((d & 2) && !(mode & 2)) tb_acc = 0; mode = d; if (d & 0x10) status &= ~1; if (d & 0x20) status &= ~2; }
        else if (part == 0 && r == 0x2A) {
            timer_run();
            if (pend_ovf) { long long dl = zcyc - lastovf; if (dl > maxdelay && dacn > 4) maxdelay = dl; pend_ovf = 0; }
            if ((uint8_t)(d) != (uint8_t)(dacn % 255 + 1)) bad++;
            dacn++;
        } else if (r >= 0x30) { writes++; if (d != (uint8_t)(r ^ 0x5A)) bad++; }
        return;
    }
    if (a == 0x7F11) psgn++;
}
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb"); if (!f) return 1; size_t n = fread(ram, 1, 0x2000, f); fclose(f); (void)n;
    int units = atoi(argv[2]); double secs = atof(argv[3]); int K = atoi(argv[4]); int hold = argc > 5 ? atoi(argv[5]) : 0;
    int tav = 1024 - units; ram[0x1F00] = tav & 255; ram[0x1F01] = tav >> 8;
    Cz80_Init(&z); Cz80_Set_ReadB(&z, rd); Cz80_Set_WriteB(&z, wr); Cz80_Set_Fetch(&z, 0, 0x1FFF, (FPTR)ram); Cz80_Reset(&z);
    long long total = (long long)(secs * 3579545), vbl = 59736, nv = vbl;
    while (zcyc < total) {
        zcyc += Cz80_Exec(&z, 8);
        if (zcyc >= nv) {
            nv += vbl;
            /* the 68000: entries up to 300 ahead of the Z80's next one; the bus held meanwhile */
            uint16_t drd = ram[0x1F02] | ram[0x1F03] << 8; uint32_t rdi = (drd - 0x1000) / 4;
            uint32_t used = (w68 - rdi) & 511, put = 0;
            while (used < 300) {
                uint8_t *e = ram + 0x1000 + (w68 & 511) * 4;
                if (e[0]) break;
                uint8_t fl = 0;
                if (K && w68 % K == 0) { uint8_t r = 0x30 + (w68 / K) % 0x80; fl = (w68 / K) % 7 == 3 ? 4 : 2 + ((w68 / K) & 1); e[2] = r; e[3] = r ^ 0x5A; }
                e[1] = fl; e[0] = (uint8_t)(w68 % 255 + 1); w68++; used++; put++;
            }
            if (hold) {  /* bus holds: the Z80 stops (pieces of 16 bytes ~ 60 us each, a few us apart) */
                for (int b = 0; b < hold; b += 16) { long long st = (long long)(60e-6 * 3579545); zcyc += st; timer_run(); zcyc += Cz80_Exec(&z, 30); }
            }
        }
    }
    printf("debt now %d; ", ram[0x1F07]);
    printf("units %d K %d hold %d: overflows %ld missed %ld dac %ld writes %ld psg %ld bad %ld max delay %lld cycles, dry %d\n",
           units, K, hold, ovf, missed, dacn, writes, psgn, bad, maxdelay, ram[0x1F04] & 1);
    return 0;
}
