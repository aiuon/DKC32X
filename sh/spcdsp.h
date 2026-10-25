/* S-DSP subset for the 32X music player (portable C: also built on the PC for tests).
 *
 * Voices play pre-decoded BRR samples (16-bit, pre-filtered like the S-DSP gaussian), resampled with
 * linear interpolation at the output rate; ADSR/GAIN envelopes follow blargg's SPC_DSP rules at the
 * DSP's 32 kHz step rate; echo = delay line + FIR + feedback like the real chip.
 */
#ifndef SPCDSP_H
#define SPCDSP_H
#include <stdint.h>

typedef struct {            /* lives in ROM (music pack, big-endian = native on the SH-2) */
    uint32_t off_pcm;       /* int16 samples (+1 pad sample after the end), from spcdsp_t.base */
    uint32_t len;           /* samples */
    uint32_t loop;          /* loop start (== len: no loop) */
} dsp_sample_t;

typedef struct {
    const int16_t *pcm;
    uint32_t pos, end, looplen;         /* 16.16 sample positions */
    int32_t env, hidden;                /* 0..0x7FF */
    int32_t gl, gr;                     /* mixing gains reached at the end of the last mix call */
    int32_t cnt;                        /* envelope countdown (32 kHz ticks << 8) */
    uint8_t mode, on, pad1, pad2;
    int32_t ngl, ngr, ndgl, ndgr;       /* noise voices: gains through the current mix call */
} dsp_voice_t;

typedef struct {
    uint8_t regs[128];
    dsp_voice_t v[8];
    const dsp_sample_t *const *dir;     /* by SRCN (0 = none) */
    const uint8_t *base;                /* sample data base */
    uint32_t nsrc;
    uint32_t rate;                      /* output Hz */
    uint32_t k32;                       /* 32 kHz ticks per output sample, 16.16 */
    uint32_t pitchmul;                  /* step = pitch * pitchmul >> 12 (16.16 result) */
    int8_t *echo;                       /* echo line at half rate, stereo 8-bit (>> 8); cap + 8 entries */
    uint32_t echo_cap, echo_len, echo_pos;
    uint32_t fir_delay;                 /* FIR taps 0/1 sit 7/6 S-DSP samples behind: extra line length */
    int32_t pwm_center;                 /* SH-2 output: PWM pulse width of silence */
    int32_t mvl, mvr;                   /* master volume (folded into the voice gains) */
    int32_t half;                       /* 1: voices mixed at half rate (light mode) */
    int32_t *work;                      /* 0, or 2 * CHUNK * 2 longs of fast RAM for the mix buffers */
    uint32_t smp_or;                    /* or-ed into sample addresses (0x20000000: read uncached) */
    int32_t hprev[4];
    uint32_t t32;                       /* 32 kHz time (<<8) */
    int32_t lpk;                        /* (unused) */
    int32_t lpl, lpr;                   /* output one-pole lowpass state (k = 7/8), amp16 << 13 */
    uint32_t lfsr, ncnt;                /* noise generator (NON voices): 15-bit LFSR, rate counter */
} spcdsp_t;

void spcdsp_init(spcdsp_t *d, uint32_t rate, const dsp_sample_t *const *dir, uint32_t nsrc,
                 const uint8_t *base, int8_t *echo_buf, uint32_t echo_cap_frames);
void spcdsp_write(spcdsp_t *d, int reg, int val);
/* mix n stereo frames.  SH-2: out = n longs of PWM pulse widths (L << 16 | R);
   elsewhere: out = 2n int16 samples */
void spcdsp_mix(spcdsp_t *d, void *out, int n);

#endif
