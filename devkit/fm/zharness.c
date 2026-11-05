/* (I24, devkit) SOUND FM Z80 driver harness: z80/fmdrv.bin on a Z80 core (cz80) with a YM2612 timer model,
   the 68000's part done here (tables, ring feeding every vblank, the start), every YM / PSG write logged
   with its time and the driver's tick number; optional audio through PicoDrive's YM2612 / SN76489.
   zharness fmdrv.bin song.feed seconds writes.log [out.raw (s16 stereo at 53267 Hz)]
   feed (tools/audio/fm/fmexport.py): "FMF1" tk(u32) flags(u32) delay(u32) ninst nenv (u32 each),
   inst[11 n], env[7 n], 7 x (u32 len, bytes) - the expanded streams (loops unrolled) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "cz80.h"

static uint8_t ram[0x2000];
static double ym_clk_per_z = 7670453.0 / 3579545.0;
static long long zcyc;                 /* Z80 cycles run */
static int ym_addr[2], ta = 0, mode = 0, status = 0;
static double ta_acc;
static long missed, ovf;
static long misspc[0x2000];
static cz80_struc z;
static FILE *logf_;
static int audio;
extern void YM2612Init_(int, int, int); extern void YM2612ResetChip_(void); extern int YM2612Write_(unsigned, unsigned char);
extern int YM2612UpdateOne_(int *, int, int, int); extern void SN76496_init(int, int); extern void SN76496Write(int);
extern void SN76496Update(short *, int, int);
static void timer_run(void)
{
    static long long last;
    double ycyc = (zcyc - last) * ym_clk_per_z; last = zcyc;
    if (!(mode & 1)) return;
    ta_acc += ycyc;
    double per = (1024 - ta) * 144.0;
    while (ta_acc >= per) { ta_acc -= per; ovf++; if (mode & 4) { if (status & 1) { missed++; misspc[Cz80_Get_Reg(&z, CZ80_PC) & 0x1FFF]++; } status |= 1; } }
}
#define POLL 0x179
static long long lastpoll; static long long gapmax[0x2000]; static long gapn[0x2000]; static int lastret, gapfrom[0x2000];
static UINT8 rd(UINT32 a)
{
    a &= 0xFFFF;
    if (a == 0x4000) {
        int pc = Cz80_Get_Reg(&z, CZ80_PC) & 0x1FFF;
        if (pc >= POLL && pc <= POLL + 3) {   /* (poll's status read) */
            int sp = Cz80_Get_Reg(&z, CZ80_SP), ret = ram[sp & 0x1FFF] | ram[(sp + 1) & 0x1FFF] << 8;
            long long g = zcyc - lastpoll; lastpoll = zcyc;
            if (g > gapmax[ret & 0x1FFF]) { gapmax[ret & 0x1FFF] = g; gapfrom[ret & 0x1FFF] = lastret; }
            lastret = ret;
            if (g > 1800) { gapn[ret & 0x1FFF]++;
                static int shown; if (zcyc > 200000 && shown++ < 2) { fprintf(logf_, "# gap %lld at %lld, recent pcs:", g, zcyc);
                    extern int pch[64]; extern int pchi; for (int q = 0; q < 64; q++) fprintf(logf_, " %04x", pch[(pchi + q) & 63]); fprintf(logf_, "\n"); } }
        }
    }
    if (a < 0x4000) return ram[a & 0x1FFF];
    if (a < 0x6000) { timer_run(); return (uint8_t)status; }
    return 0xFF;
}
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
        else if (part == 0 && r == 0x27) {
            timer_run();
            if ((d & 1) && !(mode & 1)) ta_acc = 0;
            mode = d; if (d & 0x10) status &= ~1;
        } else
            fprintf(logf_, "%lld %u %d %02x %02x\n", zcyc, ram[0x1F0C] | ram[0x1F0D] << 8, part, r, d);
        if (audio) { YM2612Write_(part * 2, (unsigned char)r); YM2612Write_(part * 2 + 1, d); }
        return;
    }
    if (a == 0x7F11) {
        fprintf(logf_, "%lld %u 2 00 %02x\n", zcyc, ram[0x1F0C] | ram[0x1F0D] << 8, d);
        if (audio) SN76496Write(d);
    }
}
static FPTR fetchp;
int pch[64]; int pchi;
static uint32_t rd32(FILE *f) { uint32_t v; if (fread(&v, 4, 1, f) != 1) exit(2); return v; }
static uint8_t *st[7]; static uint32_t stn[7], stpos[7];
static int cmdlen(const uint8_t *p)
{
    int op = p[0];
    if (op < 0x40) return 1;
    switch (op) { case 0x40: return 3; case 0x42: return 1; case 0x50: return 6; case 0x51: return 3; case 0x52: return 2;
    case 0x53: case 0x54: return 1; case 0x55: return 2; case 0x56: return p[1] + 3; case 0x57: return 2;
    case 0x58: return 2; case 0x59: case 0x5A: case 0x5B: return 1; case 0x5C: case 0x5D: case 0x5E: case 0x5F: return 2; }
    if (op >= 0x80) return 1;
    fprintf(stderr, "bad op %02x\n", op); exit(3);
}
static void feed(void)
{   /* the 68000 at a vblank: whole commands into each ring while there is room */
    for (int s = 0; s < 7; s++) {
        uint16_t w = ram[0x1F10 + 2 * s] | ram[0x1F11 + 2 * s] << 8, r = ram[0x1F20 + 2 * s] | ram[0x1F21 + 2 * s] << 8;
        for (;;) {
            if (stpos[s] >= stn[s]) break;
            int L = cmdlen(st[s] + stpos[s]);
            int room = (r - w - 1) & 511;
            if (room < L) break;
            for (int i = 0; i < L; i++) { ram[0x1000 + s * 512 + w] = st[s][stpos[s]++]; w = (w + 1) & 511; }
        }
        ram[0x1F10 + 2 * s] = w & 255; ram[0x1F11 + 2 * s] = w >> 8;
    }
}
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb"); size_t n = fread(ram, 1, 0x2000, f); fclose(f); (void)n;
    f = fopen(argv[2], "rb");
    if (rd32(f) != 0x31464D46) return 1;
    uint32_t tk = rd32(f), flags = rd32(f), delay = rd32(f), ni = rd32(f), ne = rd32(f);
    uint8_t inst[11 * 64], env[7 * 256];
    if (fread(inst, 11, ni, f) != ni || fread(env, 7, ne, f) != ne) return 1;
    for (int s = 0; s < 7; s++) { stn[s] = rd32(f); st[s] = malloc(stn[s] + 8); if (fread(st[s], 1, stn[s], f) != stn[s]) return 1; }
    fclose(f);
    double secs = atof(argv[3]);
    logf_ = fopen(argv[4], "w");
    FILE *af = argc > 5 ? fopen(argv[5], "wb") : 0;
    audio = af != 0;
    if (audio) { YM2612Init_(7670453, 53267, 0); YM2612ResetChip_(); SN76496_init(3579545, 53267); }
    Cz80_Init(&z);
    Cz80_Set_ReadB(&z, rd); Cz80_Set_WriteB(&z, wr);
    fetchp = (FPTR)ram;
    Cz80_Set_Fetch(&z, 0x0000, 0x1FFF, (FPTR)ram);
    Cz80_Reset(&z);
    long long total = (long long)(secs * 3579545), vbl = 59736, next_vbl = vbl;
    /* let the driver initialise, then the 68000's START */
    for (int i = 0; i < 2000; i++) { zcyc += Cz80_Exec(&z, 16); }
    memcpy(ram + 0x0C00, inst, 11 * ni); memcpy(ram + 0x0800, env, 7 * ne);
    for (int s = 0; s < 7; s++) { ram[0x1F10 + 2 * s] = ram[0x1F11 + 2 * s] = ram[0x1F20 + 2 * s] = ram[0x1F21 + 2 * s] = 0; }
    feed();
    ram[0x1F01] = (uint8_t)flags;
    ram[0x1F07] = tk & 255; ram[0x1F08] = (tk >> 8) & 255; ram[0x1F09] = (tk >> 16) & 255;
    uint32_t acc = tk - delay * 256; ram[0x1F04] = acc & 255; ram[0x1F05] = (acc >> 8) & 255; ram[0x1F06] = acc >> 16;
    ram[0x1F02] = ram[0x1F03] = 0;
    ram[0x1F00] = 1;
    long long start = zcyc;
    fprintf(logf_, "# start %lld\n", start);
    double ysmp = 0; long long ylast = zcyc;
    while (zcyc - start < total) {
        zcyc += Cz80_Exec(&z, 16);
        pch[pchi++ & 63] = Cz80_Get_Reg(&z, CZ80_PC); pchi &= 63;
        if (zcyc >= next_vbl) { next_vbl += vbl; feed(); }
        if (audio) {
            ysmp += (zcyc - ylast) * 53267.0 / 3579545; ylast = zcyc;
            while (ysmp >= 1) {
                int b[2] = { 0, 0 }; short p[2] = { 0, 0 };
                YM2612UpdateOne_(b, 1, 1, 1); SN76496Update(p, 1, 1);
                short o[2] = { (short)(b[0] + p[0] > 32767 ? 32767 : b[0] + p[0] < -32768 ? -32768 : b[0] + p[0]),
                               (short)(b[1] + p[1] > 32767 ? 32767 : b[1] + p[1] < -32768 ? -32768 : b[1] + p[1]) };
                fwrite(o, 2, 2, af); ysmp -= 1;
            }
        }
    }
    fprintf(logf_, "# overflows %ld missed %ld\n", ovf, missed);
    for (int i = 0; i < 0x2000; i++) if (misspc[i]) fprintf(logf_, "# miss pc %04x %ld\n", i, misspc[i]);
    for (int i = 0; i < 0x2000; i++) if (gapn[i]) fprintf(logf_, "# poll from %04x: max gap before %lld (previous poll from %04x), %ld over 1800\n", i, gapmax[i], gapfrom[i], gapn[i]);
    fprintf(logf_, "# end ticks %u stat %02x alive %u\n", ram[0x1F0C] | ram[0x1F0D] << 8, ram[0x1F0B], ram[0x1F0A]);
    fclose(logf_);
    if (af) fclose(af);
    return 0;
}
