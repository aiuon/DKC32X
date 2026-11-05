/* (I24) PC tool: play a song of a little-endian music pack (mkmusic.py ROM out.bin le) through the 32X music core
   and log every S-DSP register write with its time - the input of tools/audio/fmconv.py (SOUND FM).
   cc -O2 -DDSP_HOOK -DMP_VMASK_ALL=fmdump_vmask -Ish tools/audio/fmdump.c sh/musplay.c sh/spcdsp.c build/spceng_gen.c -o fmdump
   ./fmdump music_le.bin song seconds out.log [wav]
   log: "t reg val" (t = 32 kHz samples, hex reg / val); first lines "# srcn sample" = the song's sample map */
#include <stdio.h>
#include <stdlib.h>
#include "musplay.h"

static FILE *logf_;
int fmdump_vmask = 0xFF;             /* (FMDUMP_VMASK=hex: mix only these voices into the wav) */
static unsigned long now;
void dsp_hook(int reg, int val) { fprintf(logf_, "%lu %02X %02X\n", now, reg & 0x7F, val & 0xFF); }

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *blob = malloc(n);
    if (fread(blob, 1, n, f) != (size_t)n) return 1;
    fclose(f);
    int song = atoi(argv[2]);
    if (getenv("FMDUMP_VMASK")) fmdump_vmask = (int)strtol(getenv("FMDUMP_VMASK"), 0, 16);
    /* (FMDUMP_SRCN=hex 64-bit mask: mix only voices playing these samples - SOUND FM's 32X part) */
    unsigned long long srcn_mask = getenv("FMDUMP_SRCN") ? strtoull(getenv("FMDUMP_SRCN"), 0, 16) : 0;
    const uint32_t rate = 32000;
    uint32_t total = (uint32_t)(atof(argv[3]) * rate);
    static int8_t echo[4096 * 2];
    static musplay_t m;
    logf_ = fopen(argv[4], "w");
    {   /* the song's SRCN -> global sample index (mus_song_t.smp) */
        const mus_hdr_t *h = (const mus_hdr_t *)blob;
        const mus_song_t *sg = (const mus_song_t *)(blob + h->off_songs) + song;
        for (int i = 0; i < MUS_NSRC; i++) if (sg->smp[i] != 0xFFFF) fprintf(logf_, "# %d %d\n", i, sg->smp[i]);
    }
    mp_start(&m, blob, song, echo, (4 * 512 * rate + 31999) / 32000, rate);
    int16_t *out = malloc(total * 4);
    for (uint32_t done = 0; done < total; done += 32) {
        now = done;
        if (srcn_mask) {
            int vm = 0;
            for (int i = 0; i < 8; i++) if (srcn_mask >> (m.dsp.regs[i * 16 + 4] & 63) & 1) vm |= 1 << i;
            fmdump_vmask = vm;
        }
        mp_render(&m, out + done * 2, total - done < 32 ? total - done : 32);
    }
    fclose(logf_);
    if (argc > 5) {
        f = fopen(argv[5], "wb");
        uint32_t bytes = total * 4;
        uint32_t hdr[11] = { 0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16, 0x00020001, rate, rate * 4,
                             0x00100004, 0x61746164, bytes };
        fwrite(hdr, 4, 11, f); fwrite(out, 4, total, f); fclose(f);
    }
    return 0;
}
