/* S-DSP subset: see spcdsp.h */
#include "spcdsp.h"

enum { ENV_RELEASE, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN };

/* The SH-2 has no multi-bit arithmetic shift: GCC calls a libgcc routine for every signed x >> n
   (n != 1, 16).  Use these instead. */
#ifdef __sh__
static inline int32_t mulhi(int32_t a, int32_t b)          /* (a * b) >> 32 : dmuls.l */
{
    int32_t r;
    __asm__("dmuls.l %1,%2\n\tsts mach,%0" : "=r"(r) : "r"(a), "r"(b) : "mach", "macl");
    return r;
}
#else
static inline int32_t mulhi(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * b) >> 32);
}
#endif
static inline int32_t sar8(int32_t x)                      /* floor(x / 256) */
{
    return (int32_t)(((uint32_t)x + 0x80000000u) >> 8) - 0x800000;
}

/* envelope step periods in 32 kHz samples (SPC_DSP counter_rates; 0 = never) */
static const uint16_t env_period[32] = {
    0, 2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256, 192, 160, 128, 96, 80,
    64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1
};

#define VREG(v, r) d->regs[(v) * 16 + (r)]

void spcdsp_init(spcdsp_t *d, uint32_t rate, const dsp_sample_t *const *dir, uint32_t nsrc,
                 const uint8_t *base, int8_t *echo_buf, uint32_t echo_cap_frames)
{
    uint8_t *p = (uint8_t *)d;
    for (uint32_t i = 0; i < sizeof(*d); i++) p[i] = 0;
    d->rate = rate;
    d->dir = dir;
    d->nsrc = nsrc;
    d->base = base;
    d->k32 = (uint32_t)(((uint64_t)32000 << 16) / rate);
    d->pitchmul = (uint32_t)(((uint64_t)32000 << 16) / rate);   /* step16 = pitch * pitchmul >> 12 */
    d->echo = echo_buf;
    d->echo_cap = echo_cap_frames;
    d->fir_delay = (7 * rate / 2 + 16000) / 32000;
    d->regs[0x6C] = 0xE0;
    d->lpk = 256;
    d->lfsr = 0x4000;
}

static void key_on(spcdsp_t *d, int i)
{
    dsp_voice_t *v = &d->v[i];
    uint32_t src = VREG(i, 4);
    const dsp_sample_t *smp = src < d->nsrc ? d->dir[src] : 0;
    if (!smp && !(d->regs[0x3D] & (1 << i))) { v->on = 0; return; }
    if (!smp) smp = d->dir[0] ? d->dir[0] : 0;   /* (noise voice: the sample is not played) */
    if (!smp) { v->pcm = 0; v->pos = 0; v->end = 1u << 30; v->looplen = 1u << 30;
                v->env = 0; v->hidden = 0; v->gl = v->gr = 0; v->mode = ENV_ATTACK; v->cnt = 0; v->on = 1; return; }
    v->pcm = (const int16_t *)(d->base + smp->off_pcm);
    v->pos = 0;
    v->end = smp->len << 16;
    v->looplen = (smp->len - smp->loop) << 16;
#ifdef FAKESMP
    {   /* test: play from a small SDRAM buffer (no ROM reads) */
        extern int16_t fake_smp[];
        v->pcm = fake_smp; v->end = 60 << 16; v->looplen = 50 << 16;
    }
#endif
    v->env = 0;
    v->hidden = 0;
    v->gl = v->gr = 0;
    v->mode = ENV_ATTACK;
    v->cnt = 0;
    v->on = 1;
}

void spcdsp_write(spcdsp_t *d, int reg, int val)
{
    reg &= 0x7F;
    d->regs[reg] = (uint8_t)val;
    if (reg == 0x4C) {                       /* KON */
        for (int i = 0; i < 8; i++)
            if (val & (1 << i)) key_on(d, i);
    } else if (reg == 0x5C) {                /* KOF */
        for (int i = 0; i < 8; i++)
            if (val & (1 << i)) d->v[i].mode = ENV_RELEASE;
    } else if (reg == 0x6C) {
        if (val & 0x80)
            for (int i = 0; i < 8; i++) { d->v[i].mode = ENV_RELEASE; d->v[i].env = 0; }
    } else if (reg == 0x7D) {
        uint32_t frames = ((uint32_t)(val & 15) * 512 * d->rate + 16000) / 32000;   /* 16 ms per step */
        frames >>= 1;                        /* the echo runs at half the output rate */
        if (frames > d->echo_cap) frames = d->echo_cap;
        if (!frames) frames = 1;
        d->echo_len = frames + d->fir_delay;
        if (d->echo_pos >= frames) d->echo_pos = 0;
    }
}

/* advance a voice's envelope by 'ticks' 32 kHz samples (<<8 fraction) */
static void run_env(spcdsp_t *d, int i, int32_t ticks)
{
    dsp_voice_t *v = &d->v[i];
    if (v->mode == ENV_RELEASE) {
        int32_t e = v->env - (int32_t)((uint32_t)ticks >> 5);
        v->env = e < 0 ? 0 : e;
        if (!v->env) v->on = 0;
        return;
    }
    if (!(VREG(i, 5) & 0x80) && VREG(i, 7) < 0x80) {
        /* GAIN direct: the level is simply the register (rate 31) */
        v->env = VREG(i, 7) * 0x10;
        v->hidden = v->env;
        v->cnt = 0;
        return;
    }
    v->cnt -= ticks;
    while (v->cnt <= 0) {
        unsigned adsr0 = VREG(i, 5), env_data, rate;
        int env = v->env;
        if (adsr0 & 0x80) {
            env_data = VREG(i, 6);
            if (v->mode >= ENV_DECAY) {
                env--;
                env -= sar8(env);
                rate = env_data & 0x1F;
                if (v->mode == ENV_DECAY) rate = (adsr0 >> 3 & 0x0E) + 0x10;
            } else {
                rate = (adsr0 & 0x0F) * 2 + 1;
                env += rate < 31 ? 0x20 : 0x400;
            }
        } else {
            unsigned mode;
            env_data = VREG(i, 7);
            mode = env_data >> 5;
            if (mode < 4) { env = env_data * 0x10; rate = 31; }
            else {
                rate = env_data & 0x1F;
                if (mode == 4) env -= 0x20;
                else if (mode < 6) { env--; env -= sar8(env); }
                else {
                    env += 0x20;
                    if (mode > 6 && (unsigned)v->hidden >= 0x600) env += 0x8 - 0x20;
                }
            }
        }
        if (sar8(env) == (int)(env_data >> 5) && v->mode == ENV_DECAY) v->mode = ENV_SUSTAIN;
        v->hidden = env;
        if ((unsigned)env > 0x7FF) {
            env = env < 0 ? 0 : 0x7FF;
            if (v->mode == ENV_ATTACK) v->mode = ENV_DECAY;
        }
        if (!env_period[rate]) { v->cnt = 0x7FFFFFFF; break; }
        int same = env == v->env;
        v->env = env;
        v->cnt += env_period[rate] << 8;
        if (same && v->mode != ENV_ATTACK && (env == 0 || env == 0x7FF)) {
            if (v->cnt <= 0) v->cnt = env_period[rate] << 8;   /* nothing changes: skip ahead */
            break;
        }
    }
}

#define CHUNK 32

#if defined(MUSDEBUG) && defined(__sh__)
static inline uint16_t frt(void)
{
    uint32_t h = *(volatile uint8_t *)0xFFFFFE12;
    return (uint16_t)(h << 8 | *(volatile uint8_t *)0xFFFFFE13);
}
volatile uint32_t dsp_prof[4];
#define PROF_T(v) uint16_t v = frt()
#define PROF_ADD(k, t0) (dsp_prof[k] += (uint16_t)(frt() - (t0)))
#else
#define PROF_T(v)
#define PROF_ADD(k, t0)
#endif

typedef struct { uint32_t step, n; int32_t gl, gr; uint32_t end, looplen; int32_t dgl, dgr; } mixv_args_t;
#ifdef __sh__
uint32_t mixv(int32_t *acc, const int16_t *pcm, uint32_t pos, mixv_args_t *a);
#else
static uint32_t mixv(int32_t *acc, const int16_t *pcm, uint32_t pos, mixv_args_t *a)
{
    for (uint32_t j = 0; j < a->n; j++) {
        uint32_t k = pos >> 16;
        int32_t s0 = pcm[k], s1 = pcm[k + 1];
        int32_t s = s0 + (int16_t)((((s1 - s0) * (int32_t)((pos & 0xFFFF) >> 1)) << 1) >> 16);
        acc[2 * j] += s * (int16_t)a->gl;
        acc[2 * j + 1] += s * (int16_t)a->gr;
        a->gl += a->dgl;
        a->gr += a->dgr;
        pos += a->step;
        if (pos >= a->end) {
            if (!a->looplen) return 0xFFFFFFFF;
            pos -= a->looplen;
        }
    }
    return pos;
}
#endif

typedef struct { mixv_args_t a; const int16_t *pcm; uint32_t pos; } vjob_t;

#ifdef __sh__
void mixjobs(int32_t *acc, int32_t *ein, vjob_t *jobs, uint32_t counts);
#else
static void mixjobs(int32_t *acc, int32_t *ein, vjob_t *jobs, uint32_t counts)
{
    uint32_t ne = counts & 0xFF, nt = counts >> 8 & 0xFF, m = counts >> 16 & 0xFF;
    for (uint32_t j = 0; j < m * 2; j++) acc[j] = 0;
    for (uint32_t k = 0; k < nt; k++) {
        if (k == ne && (counts >> 24))
            for (uint32_t j = 0; j < m * 2; j++) ein[j] = acc[j];
        if (jobs[k].pos == 0xFFFFFFFF) continue;
        jobs[k].a.n = m;
        jobs[k].pos = mixv(acc, jobs[k].pcm, jobs[k].pos, &jobs[k].a);
    }
    if (ne == nt && (counts >> 24))
        for (uint32_t j = 0; j < m * 2; j++) ein[j] = acc[j];
}
#endif

#ifdef __sh__
void expand2(int32_t *buf, uint32_t nh, int32_t *prev);
#else
static void expand2(int32_t *buf, uint32_t nh, int32_t *prev)
{
    int32_t lastl = buf[2 * nh - 2], lastr = buf[2 * nh - 1];
    for (int k = (int)nh - 1; k >= 0; k--) {
        int32_t al = buf[2 * k], ar = buf[2 * k + 1];
        int32_t pl = k ? buf[2 * k - 2] : prev[0], pr = k ? buf[2 * k - 1] : prev[1];
        buf[4 * k + 2] = al; buf[4 * k + 3] = ar;
        buf[4 * k] = (pl + al) >> 1; buf[4 * k + 1] = (pr + ar) >> 1;
    }
    prev[0] = lastl; prev[1] = lastr;
}
#endif

/* envelope and mixing parameters of voice i for the next 'frames' voice frames ('ticks' 32 kHz
   samples): gains ramp from where the last call ended to the new envelope; 0 if silent */
/* (I12: SDRAM - the slave runs it for the music, ~5 cycles per instruction from the cartridge) */
__attribute__((section(".sdtext"), aligned(16))) static int voice_job(spcdsp_t *d, int i, vjob_t *jb, int frames, int32_t ticks, int half)
{
    dsp_voice_t *v = &d->v[i];
    run_env(d, i, ticks);
    /* S-DSP: amp16 = ((s16 * env) >> 11) * vol >> 7, main = amp16 * mvol >> 7.  Here s15 = s16 / 2
       and g = env * vol * mvol >> 11 (master volume folded in): acc += s15 * g -> main = acc >> 13 */
    int32_t env = v->on ? v->env : 0;
    int32_t gl = mulhi(env * (int8_t)VREG(i, 0) * d->mvl, 1 << 21);
    int32_t gr = mulhi(env * (int8_t)VREG(i, 1) * d->mvr, 1 << 21);
    if (!gl && !gr && !v->gl && !v->gr) return 0;
    uint32_t pitch = (VREG(i, 2) | VREG(i, 3) << 8) & 0x3FFF;
    jb->a.step = (pitch * d->pitchmul) >> 12;
#ifdef STEPHIST
    { extern unsigned long step_hist[17]; unsigned b = jb->a.step >> 13; step_hist[b > 16 ? 16 : b] += frames; }
#endif
    if (half) jb->a.step += jb->a.step;
    jb->a.gl = v->gl;
    jb->a.gr = v->gr;
    jb->a.dgl = (gl - v->gl) / frames;
    jb->a.dgr = (gr - v->gr) / frames;
    v->gl = gl;
    v->gr = gr;
    jb->a.end = v->end;
    jb->a.looplen = v->looplen;
    jb->pcm = (const int16_t *)((uintptr_t)v->pcm | d->smp_or);
    jb->pos = v->pos;
    return 1;
}

static inline int32_t clamp16(int32_t x)
{
    return x > 32767 ? 32767 : x < -32768 ? -32768 : x;
}

typedef struct {
    uint32_t n;
    int32_t kinl, kinr, evl, evr, efb, c0, c1;
    int8_t *ebase, *epos, *eend;
    uint32_t flags;
    int32_t pmax, lpl, lpr, center;
} mixout_t;

#ifdef __sh__
void mixout(const int32_t *acc, const int32_t *ein, uint32_t *out, mixout_t *st);
#else
static inline int32_t sar3(int32_t x) { return x >> 3; }
/* C model of sh/mixout.s, 16-bit output */
static void mixout_c(const int32_t *acc, const int32_t *ein, int16_t *out, mixout_t *st)
{
    for (uint32_t j = 0; j < st->n; j += 2, acc += 4, ein += 4, out += 4) {
        int32_t eo[2] = { 0, 0 };
        if (st->flags & 1) {
            int8_t *t1 = st->epos + 2 == st->eend ? st->ebase : st->epos + 2;
            for (int c = 0; c < 2; c++) {
                int32_t sum = (st->epos[c] * st->c0 + t1[c] * st->c1) << 8;
                eo[c] = mulhi(sum, c ? st->evr : st->evl);
                if (st->flags & 2) {
                    int32_t w = mulhi(sum, st->efb) + mulhi(ein[c] + ein[c + 2], c ? st->kinr : st->kinl);
                    w = w > 0x7F7F ? 0x7F7F : w < -32768 ? -32768 : w;
                    st->epos[c] = (int8_t)((w + 0x80) >> 8);
                }
            }
            st->epos = t1;
        }
        for (int f = 0; f < 2; f++) {
            int32_t l = acc[f * 2] + (eo[0] << 13), r = acc[f * 2 + 1] + (eo[1] << 13);
            int32_t dl = l - st->lpl, dr = r - st->lpr;
            st->lpl += dl - sar3(dl);
            st->lpr += dr - sar3(dr);
            out[f * 2] = (int16_t)clamp16(st->lpl >> 13);
            out[f * 2 + 1] = (int16_t)clamp16(st->lpr >> 13);
        }
    }
}
#endif

/* NON voices: the S-DSP replaces the sample by its noise generator (a 15-bit LFSR clocked at the
   FLG & $1F rate, same rate table as the envelopes); envelope and volumes as for a sample voice.
   Mixed here in C (rare: sound effects such as Slippa's hiss), into acc (+ ein if echo-enabled). */
static void noise_voices(spcdsp_t *d, unsigned non, unsigned eon, int32_t *acc, int32_t *ein, int m,
                         int32_t ticks_total, int frames_total, int half)
{
    uint32_t period = env_period[d->regs[0x6C] & 0x1F];
    uint32_t step = half ? d->k32 * 2 : d->k32;        /* 32 kHz ticks per mixed frame, 16.16 */
    uint32_t lfsr = d->lfsr, cnt = d->ncnt;
    int16_t smp[CHUNK];
    for (int j = 0; j < m; j++) {
        if (period) {
            cnt += step;
            while (cnt >= period << 16) {
                cnt -= period << 16;
                lfsr = (lfsr >> 1) | (((lfsr << 14) ^ (lfsr << 13)) & 0x4000);
            }
        }
        smp[j] = (int16_t)((int16_t)(lfsr << 1) >> 1);     /* 15-bit, like the pre-decoded samples */
    }
    d->lfsr = lfsr; d->ncnt = cnt;
    (void)ticks_total; (void)frames_total;
    for (int i = 0; i < 8; i++) {
        if (!(non & (1 << i))) continue;
        dsp_voice_t *v = &d->v[i];
        if (!v->on && !v->gl && !v->gr) continue;
        int32_t gl = v->ngl, gr = v->ngr;
        int32_t dgl = v->ndgl, dgr = v->ndgr;
        for (int j = 0; j < m; j++) {
            int32_t s = smp[j];
            acc[2 * j] += s * (int16_t)gl;
            acc[2 * j + 1] += s * (int16_t)gr;
            if (eon & (1 << i)) { ein[2 * j] += s * (int16_t)gl; ein[2 * j + 1] += s * (int16_t)gr; }
            gl += dgl; gr += dgr;
        }
        v->ngl = gl; v->ngr = gr;
    }
}

/* (I12: SDRAM - the slave runs it for the music, ~5 cycles per instruction from the cartridge) */
__attribute__((section(".sdtext"), aligned(16))) void spcdsp_mix(spcdsp_t *d, void *outp, int n)
{
    mixout_t st;
    int mute = d->regs[0x6C] & 0x40;
    int32_t mvl = mute ? 0 : (int8_t)d->regs[0x0C], mvr = mute ? 0 : (int8_t)d->regs[0x1C];
    d->mvl = mvl;
    d->mvr = mvr;
    st.kinl = mvl ? (1 << 25) / mvl : 0;
    st.kinr = mvr ? (1 << 25) / mvr : 0;
    st.evl = mute ? 0 : (int8_t)d->regs[0x2C] << 18;
    st.evr = mute ? 0 : (int8_t)d->regs[0x3C] << 18;
    st.efb = (int8_t)d->regs[0x0D] << 18;
    /* FIR: the S-DSP runs 8 taps on the last 8 echo samples (tap 0 = oldest).  DKC uses
       127, 10 (or 8), 1, 1, ...: the +-1 taps are inaudible, so only taps 0 and 1 are kept. */
    st.c0 = (int8_t)d->regs[0x0F];
    st.c1 = (int8_t)d->regs[0x1F];
    unsigned eon = d->regs[0x4D];
    int half = d->half;
    int echo_on = d->echo && d->echo_len && (st.evl || st.evr || !(d->regs[0x6C] & 0x20));
    st.flags = (echo_on ? 1 : 0) | (!(d->regs[0x6C] & 0x20) && mvl && mvr ? 2 : 0);
    st.ebase = d->echo;
    st.eend = d->echo + d->echo_len * 2;
    st.epos = d->echo + d->echo_pos * 2;
    st.lpl = d->lpl;
    st.lpr = d->lpr;
    st.center = d->pwm_center - 4096;
    st.pmax = d->pwm_center * 2 - 1;
    /* envelopes and job parameters once per call (C in ROM), then the chunks (asm in SDRAM);
       echo-enabled voices first: their sum is the echo input */
    PROF_T(pt0);
    static vjob_t jobs[8];                /* (static: the slave's small stack, one interrupt at a time) */
    uint8_t who[8];
    unsigned non = d->regs[0x3D];
    int32_t ticks = (int32_t)((d->k32 * (uint32_t)n) >> 8);
    int vframes = half ? n >> 1 : n;
    uint32_t nj = 0, ne;
    unsigned bit = 1;
    for (int i = 0; i < 8; i++, bit += bit)
        if ((eon & bit) && !(non & bit) && (d->v[i].on || d->v[i].gl || d->v[i].gr) && voice_job(d, i, &jobs[nj], vframes, ticks, half))
            who[nj++] = i;
    ne = nj;
    bit = 1;
    for (int i = 0; i < 8; i++, bit += bit)
        if (!(eon & bit) && !(non & bit) && (d->v[i].on || d->v[i].gl || d->v[i].gr) && voice_job(d, i, &jobs[nj], vframes, ticks, half))
            who[nj++] = i;
    if (non) {
        bit = 1;
        for (int i = 0; i < 8; i++, bit += bit) {
            if (!(non & bit)) continue;
            dsp_voice_t *v = &d->v[i];
            vjob_t tmp;
            if ((v->on || v->gl || v->gr) && voice_job(d, i, &tmp, vframes, ticks, half)) {
                v->ngl = tmp.a.gl; v->ngr = tmp.a.gr; v->ndgl = tmp.a.dgl; v->ndgr = tmp.a.dgr;
            } else { v->ngl = v->ngr = v->ndgl = v->ndgr = 0; non &= ~bit; }
        }
    }
    PROF_ADD(2, pt0);
    while (n > 0) {
        int m = n < CHUNK ? n : CHUNK;
        /* (the work buffers are in the cache RAM; before the cache switch - the first frame - silence:
           no 512 bytes of stack or SDRAM for them) */
        /* (static: the slave's stack is 2 KB and this runs in its DMA interrupt on top of the canvas
           jobs; the cache RAM is used instead in the levels that switch the cache to two-way) */
        static int32_t accs[CHUNK * 2], eins[CHUNK * 2];
        int32_t *acc = d->work ? d->work : accs, *ein = d->work ? d->work + CHUNK * 2 : eins;
        PROF_T(pt1);
        mixjobs(acc, ein, jobs, ne | nj << 8 | (uint32_t)(half ? m >> 1 : m) << 16 | (echo_on ? 1u << 24 : 0));
        if (non) noise_voices(d, non, echo_on ? eon : 0, acc, ein, half ? m >> 1 : m, ticks, vframes, half);
        if (half) {
            expand2(acc, m >> 1, d->hprev);
            if (echo_on) expand2(ein, m >> 1, d->hprev + 2);
        }
        PROF_ADD(0, pt1);
        PROF_T(pt2);
        st.n = m;
#ifdef __sh__
        mixout(acc, ein, (uint32_t *)outp, &st);
        outp = (uint32_t *)outp + m;
#else
        mixout_c(acc, ein, (int16_t *)outp, &st);
        outp = (int16_t *)outp + m * 2;
#endif
        PROF_ADD(1, pt2);
        n -= m;
    }
    for (uint32_t k = 0; k < nj; k++) {
        dsp_voice_t *v = &d->v[who[k]];
        if (jobs[k].pos == 0xFFFFFFFF) { v->on = 0; v->gl = v->gr = 0; }
        else v->pos = jobs[k].pos;
    }
    d->echo_pos = (uint32_t)(st.epos - d->echo) / 2;
    d->lpl = st.lpl;
    d->lpr = st.lpr;
}
