/* see musplay.h */
#include "musplay.h"

#ifdef MUSDEBUG
static inline uint16_t frt(void)
{
    uint32_t h = *(volatile uint8_t *)0xFFFFFE12;
    return (uint16_t)(h << 8 | *(volatile uint8_t *)0xFFFFFE13);
}
volatile uint32_t mp_eng_ticks;
#endif

static void dsp_cb(spc_t *s, int reg, int val)
{
    spcdsp_write((spcdsp_t *)s->user, reg, val);
}

void mp_start(musplay_t *m, const uint8_t *blob, int song, int8_t *echo, uint32_t echo_cap, uint32_t rate)
{
    const mus_hdr_t *h = (const mus_hdr_t *)blob;
    uint8_t *p = (uint8_t *)m;
    for (uint32_t i = 0; i < sizeof(*m); i++) p[i] = 0;
    for (uint32_t i = 0; i < (echo_cap + 8) * 2; i++) echo[i] = 0;
    spcdsp_init(&m->dsp, rate, m->dir, MUS_NSRC, blob, echo, echo_cap);
    m->fpt = (uint32_t)(((uint64_t)rate << 16) / 32000);
    if (song >= h->nsongs) return;
    const mus_song_t *sg = (const mus_song_t *)(blob + h->off_songs) + song;
    const dsp_sample_t *smp = (const dsp_sample_t *)(blob + h->off_samples);
    for (int i = 0; i < MUS_NSRC; i++)
        m->dir[i] = sg->smp[i] == 0xFFFF ? 0 : &smp[sg->smp[i]];
    m->spc.ctab = (const uint16_t *)(blob + sg->off_img);
    m->spc.pool = blob + sg->off_ram;
    for (int i = 0; i < 0x600; i++)
        m->spc.ram[i] = m->spc.pool[((uint32_t)m->spc.ctab[i >> 6] << 6) | (i & 63)];
    m->spc.dspw = dsp_cb;
    m->spc.user = &m->dsp;
    spc_exec(&m->spc, SPC_CODE_1071);
    m->spc.ram[0x24] = 1;
    m->until = 0;
    m->playing = 1;
}

void mp_cmd(musplay_t *m, int cmd, int param)
{
    if (!m->playing) return;
    if (cmd == 0xFD) { m->spc.ram[0xED] = (uint8_t)param; return; }
    if (cmd == 0xFC) { m->spc.ram[0x25] = (uint8_t)param; return; }
    if (cmd >= 0xFE) return;
    m->spc.ram[0x4D8] = (uint8_t)param;
    m->spc.a = (uint8_t)cmd;
    m->spc.x = (uint8_t)param;
    spc_exec(&m->spc, SPC_CODE_1173);
}

/* (I12: SDRAM - the slave runs it for the music, ~5 cycles per instruction from the cartridge) */
__attribute__((section(".sdtext"), aligned(16))) void mp_render(musplay_t *m, void *out, int n)
{
    if (!m->playing) {
#ifdef __sh__
        for (int i = 0; i < n; i++) ((uint32_t *)out)[i] = m->dsp.pwm_center * 0x10001u;
#else
        for (int i = 0; i < n * 2; i++) ((int16_t *)out)[i] = 0;
#endif
        return;
    }
    while (n > 0) {
        if (m->until <= 0) {
#ifdef MUSDEBUG
            uint16_t t0 = frt();
            spc_exec(&m->spc, SPC_CODE_064F);
            mp_eng_ticks += (uint16_t)(frt() - t0);
#else
            spc_exec(&m->spc, SPC_CODE_064F);
#endif
            uint32_t ec = m->spc.ram[0xEC] ? m->spc.ram[0xEC] : 256;
            m->until += (int32_t)(4 * ec * m->fpt);
        }
        int k = (((m->until + 0xFFFF) >> 16) + 1) & ~1;   /* even: the mixer works on frame pairs */
        if (k > n) k = n;
        spcdsp_mix(&m->dsp, out, k);
#ifdef __sh__
        out = (uint32_t *)out + k;
#else
        out = (int16_t *)out + k * 2;
#endif
        n -= k;
        m->until -= k << 16;
    }
}
