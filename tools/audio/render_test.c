/* PC test: render song.py output through sh/spcdsp.c into a WAV.
   cc -O2 -I../../sh render_test.c ../../sh/spcdsp.c -o render_test
   ./render_test pre rate out.wav */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spcdsp.h"

int main(int argc, char **argv)
{
    char p[512];
    uint32_t rate = atoi(argv[2]);
    snprintf(p, sizeof p, "%s.smp", argv[1]);
    FILE *f = fopen(p, "rb");
    uint32_t n;
    fread(&n, 4, 1, f);
    dsp_sample_t *dir = calloc(n, sizeof *dir);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t h[4];
        fread(h, 4, 4, f);
        if (!h[0]) continue;
        int16_t *pcm = malloc((h[1] + 1) * 2);
        fread(pcm, 2, h[1] + 1, f);
        dir[i].pcm = pcm; dir[i].len = h[1]; dir[i].loop = h[2];
    }
    fclose(f);
    snprintf(p, sizeof p, "%s.ev", argv[1]);
    f = fopen(p, "rb");
    fseek(f, 0, SEEK_END);
    long ne = ftell(f) / 6;
    fseek(f, 0, SEEK_SET);
    unsigned char *ev = malloc(ne * 6);
    fread(ev, 6, ne, f);
    fclose(f);
    static int16_t echo[8192 * 2];
    spcdsp_t d;
    spcdsp_init(&d, rate, dir, n, echo, 8192);
    if (argc > 4) d.lpk = atoi(argv[4]);
    uint32_t last = 0;
    memcpy(&last, ev + (ne - 1) * 6, 4);
    uint32_t total = (uint32_t)((uint64_t)last * rate / 32000);
    int16_t *out = malloc(total * 4 + 64);
    uint32_t done = 0;
    long e = 0;
    while (done < total) {
        uint32_t t32;
        while (e < ne) {
            memcpy(&t32, ev + e * 6, 4);
            uint32_t at = (uint32_t)((uint64_t)t32 * rate / 32000);
            if (at > done) break;
            spcdsp_write(&d, ev[e * 6 + 4], ev[e * 6 + 5]);
            e++;
        }
        uint32_t next = total;
        if (e < ne) { memcpy(&t32, ev + e * 6, 4); next = (uint32_t)((uint64_t)t32 * rate / 32000); }
        if (next > total) next = total;
        if (next == done) next = done + 1;
        spcdsp_mix(&d, out + done * 2, next - done);
        done = next;
    }
    f = fopen(argv[3], "wb");
    uint32_t bytes = total * 4;
    uint32_t hdr[11] = { 0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16, 0x00020001, rate, rate * 4,
                         0x00100004, 0x61746164, bytes };
    fwrite(hdr, 4, 11, f);
    fwrite(out, 4, total, f);
    fclose(f);
    return 0;
}
