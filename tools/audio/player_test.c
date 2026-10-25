/* PC test of the 32X music player core (sh/musplay.c): render a song of a little-endian music pack.
   cc -O2 -Ish tools/audio/player_test.c sh/musplay.c sh/spcdsp.c sh/spceng_gen.c -o player_test
   ./player_test music_le.bin song seconds rate out.wav */
#include <stdio.h>
#include <stdlib.h>
#include "musplay.h"

#ifdef STEPHIST
unsigned long step_hist[17];
#endif
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *blob = malloc(n);
    if (fread(blob, 1, n, f) != (size_t)n) return 1;
    fclose(f);
    int song = atoi(argv[2]);
    uint32_t rate = atoi(argv[4]);
    uint32_t total = (uint32_t)(atof(argv[3]) * rate);
    static int8_t echo[4096 * 2];
    static musplay_t m;
    uint32_t cap = (4 * 512 * rate + 31999) / 32000;
    mp_start(&m, blob, song, echo, cap, rate);
    if (argc > 6) m.dsp.half = atoi(argv[6]);
    int16_t *out = malloc(total * 4);
    /* optional sound effects: argv[7..] = "sec:cmd:param" (hex cmd / param) */
    for (uint32_t done = 0; done < total; done += 128) {
        for (int k = 7; k < argc; k++) {
            double t; unsigned c, pr;
            if (sscanf(argv[k], "%lf:%x:%x", &t, &c, &pr) == 3) {
                uint32_t at = (uint32_t)(t * rate);
                if (at >= done && at < done + 128) mp_cmd(&m, c, pr);
            }
        }
        mp_render(&m, out + done * 2, total - done < 128 ? total - done : 128);
    }
#ifdef STEPHIST
    { unsigned long t = 0; for (int i = 0; i < 17; i++) t += step_hist[i];
      for (int i = 0; i < 17; i++) fprintf(stderr, "%5.1f", t ? 100.0 * step_hist[i] / t : 0.0);
      fprintf(stderr, "  (voice-frames/s %.0f)\n", (double)t / atof(argv[3])); }
#endif
    f = fopen(argv[5], "wb");
    uint32_t bytes = total * 4;
    uint32_t hdr[11] = { 0x46464952, 36 + bytes, 0x45564157, 0x20746d66, 16, 0x00020001, rate, rate * 4,
                         0x00100004, 0x61746164, bytes };
    fwrite(hdr, 4, 11, f);
    fwrite(out, 4, total, f);
    fclose(f);
    return 0;
}
