/* (I25) SOUND MD - "Genesis sound only": the 68000's part (included by main.c after fm.c).
 *
 * The 32X plays nothing.  This code is the whole sound engine: every 12.5 ms tick it runs a song's 7 FM / PSG
 * streams (the same commands as SOUND FM's Z80 driver, z80/fmdrv.asm - here turned into YM2612 / PSG
 * register writes, those that change a register only) and its PCM stream (drums / ambience), and the sound
 * effects the SH-2 asks for (0x9vxx: effect xx on S-DSP voice v - it takes the music's voice v, as on the
 * SNES); it mixes up to 8 PCM voices and fills the Z80's ring (z80/mddrv.asm): an entry per DAC sample, with
 * at most one register write.  The ring's entries ARE the time: a tick's writes start at the entry the tick
 * falls on (ticks counted like the Z80 driver does: TK per 256 x units a sample), so the drums and the FM
 * never drift apart, and the Z80 only has to keep the sample rate.
 *
 * The data (tools/audio/fm/mdconv.py, fmbuild.py; finalize.py) is in the MD page (window 5): 'MDD1' directory,
 * the Z80 player, the PCM bank (8-bit samples at the DAC rate), the effects, the songs ('FMS2' blocks: 8 LZ
 * streams - fm_next decodes them).  SH-2: 0x9B0f bit 2 = SOUND MD (with bit 0), 0x9Ann song (0xFF stop),
 * 0x90xx-0x97xx effect.  md_work (md_idle) mixes; the bus is held only per 8 entries (the Z80 keeps its rate).
 */
#pragma GCC push_options
#pragma GCC optimize ("Os")
#define MD_RING   0x1000             /* Z80: 512 entries x 4 (flag, sample, reg, val) */
#define MD_NENT   512
#define MD_MB     0x1F00             /* Z80 mailbox: 0 TAV, 2 DRD (next entry), 4 STAT, 5 ALIVE */
#define MD_CHUNK  32                 /* (mdconv.py CHUNK: a voice's end checked after <= 32 samples) */
#define MD_LEVELS 32
#define MD_FILL   300                /* entries kept ahead of the Z80 (34 ms at 8878 Hz) */
#define MD_FMQ    512                /* register writes waiting for their entries */

struct md_voice {
    uint32_t base;                   /* 68000 address of the sample (window 5) */
    uint16_t pos, frac, sfrac, sint; /* position (bank samples), step 16.16 */
    uint16_t n, ln;                  /* end, loop length (0: one-shot) */
    uint8_t on, lvl, sfx, pad;       /* sfx: an effect owns the voice */
    uint32_t sp;                     /* (effect) stream position (window-5 offset) */
    uint16_t sw;                     /* (effect) ticks to wait */
};
struct md_ch {                       /* an FM / PSG stream's channel state (fmdrv.asm ST) */
    uint16_t wait, fn;
    uint8_t vol, inst, env, pan, on, part, cc, kc;
};
static struct md_voice md_v[8];
static struct md_ch md_c[8];
static struct fm_dec md_d[8];
static int8_t md_vt[MD_LEVELS][256];           /* VOL[level][sample byte] (mdmix.py vol_table) */
static int16_t md_acc[MD_CHUNK];
static uint8_t md_clip[2048];                  /* DAC byte of a mix value -1024..1023: clip(x, -128, 127) + 128, 0 -> 1 */
static uint8_t md_q[MD_FMQ][4] __attribute__((aligned(4)));   /* type (2 part I, 3 part II, 4 PSG), reg, val
                                   (4 bytes: GCC merges the byte stores into words - a row of 3 put them at odd addresses) */
static uint16_t md_qh, md_qt;
static uint16_t md_sh[2][256];                 /* the YM2612's registers as written (0x100: unknown) */
static uint8_t md_inst[32 * 11], md_env[146 * 7];
static uint16_t md_drv, md_playing, md_units, md_att, md_sfxatt, md_page, md_w;
static uint32_t md_dir, md_bank, md_sfx, md_tk, md_ainc;
static uint32_t md_mnext, md_macc, md_snext, md_sacc, md_t;   /* (time = entries written, md_t) */
static uint16_t md_bn;
static uint32_t md_boff[64]; static uint16_t md_blen[64], md_bloop[64];
static uint32_t md_dry, md_fed, md_lines;
#ifdef MDPROF
static uint32_t md_calls, md_wsum, md_frs, md_vs;
static volatile uint16_t md_bench[2];
static volatile uint32_t md_tmix, md_twr;
static volatile uint16_t md_nsong, md_lastsong, md_nsfx;
#endif    /* (test reads: ring ran dry, entries fed, lines spent in md_work) */

static RAM_CODE void md_ym(uint8_t part, uint8_t reg, uint8_t val)
{
    if (reg != 0x28 && (reg & 0xF0) != 0xA0 && md_sh[part][reg] == val) return;
    md_sh[part][reg] = val;
    uint16_t h = md_qh;
    if (((h + 1) & (MD_FMQ - 1)) == md_qt) return;     /* (full: dropped - 512 writes is ~4 ticks' worth) */
    md_q[h][0] = (uint8_t)(2 + part); md_q[h][1] = reg; md_q[h][2] = val;
    md_qh = (h + 1) & (MD_FMQ - 1);
}
static RAM_CODE void md_psg(uint8_t b)
{
    uint16_t h = md_qh;
    if (((h + 1) & (MD_FMQ - 1)) == md_qt) return;
    md_q[h][0] = 4; md_q[h][1] = 0; md_q[h][2] = b;
    md_qh = (h + 1) & (MD_FMQ - 1);
}

/* ---- the FM streams (fmdrv.asm: c_keyon, carriers, ...) */
static const uint8_t md_slotof[4] = { 0, 8, 4, 12 };
static RAM_CODE uint8_t md_cartl(struct md_ch *c, uint8_t tl)
{
    uint16_t v = (uint16_t)tl + c->vol + md_att;
    return v > 127 ? 127 : (uint8_t)v;
}
static RAM_CODE void md_carriers(struct md_ch *c)
{
    const uint8_t *in = md_inst + c->inst * 11;
    for (uint16_t k = 0; k < 4; k++)
        if (in[1] >> k & 1) md_ym(c->part, (uint8_t)(0x40 + md_slotof[k] + c->cc), md_cartl(c, in[4 + 2 * k]));
}
static RAM_CODE void md_fnw(struct md_ch *c)
{
    md_ym(c->part, (uint8_t)(0xA4 + c->cc), (uint8_t)(c->fn >> 8));
    md_ym(c->part, (uint8_t)(0xA0 + c->cc), (uint8_t)c->fn);
}
static RAM_CODE void md_panw(struct md_ch *c)
{
    md_ym(c->part, (uint8_t)(0xB4 + c->cc), (fm_flags & 2) ? 0xC0 : (uint8_t)(c->pan << 6));
}
static RAM_CODE void md_keyon(struct md_ch *c)
{
    const uint8_t *in = md_inst + c->inst * 11, *e = md_env + c->env * 7;
    md_ym(0, 0x28, c->kc);
    md_ym(c->part, (uint8_t)(0xB0 + c->cc), in[0]);
    for (uint16_t k = 0; k < 4; k++) {
        uint8_t o = (uint8_t)(md_slotof[k] + c->cc), p = c->part;
        uint8_t mul = in[3 + 2 * k], tl = in[4 + 2 * k];
        md_ym(p, (uint8_t)(0x30 + o), mul);
        if (in[1] >> k & 1) {
            md_ym(p, (uint8_t)(0x40 + o), md_cartl(c, tl));
            md_ym(p, (uint8_t)(0x50 + o), e[0]); md_ym(p, (uint8_t)(0x60 + o), e[1]);
            md_ym(p, (uint8_t)(0x70 + o), e[2]); md_ym(p, (uint8_t)(0x80 + o), e[3]);
        } else {
            uint16_t t = (uint16_t)tl + e[6];
            md_ym(p, (uint8_t)(0x40 + o), t > 127 ? 127 : (uint8_t)t);
            if (in[2]) {                             /* (follow: the modulators fade with the note) */
                md_ym(p, (uint8_t)(0x50 + o), e[0]); md_ym(p, (uint8_t)(0x60 + o), e[4]); md_ym(p, (uint8_t)(0x70 + o), e[5]);
                md_ym(p, (uint8_t)(0x80 + o), (uint8_t)((e[4] ? 0xF0 : 0) | (e[3] & 15)));
            } else {
                md_ym(p, (uint8_t)(0x50 + o), 0x1F); md_ym(p, (uint8_t)(0x60 + o), 0); md_ym(p, (uint8_t)(0x70 + o), 0);
                md_ym(p, (uint8_t)(0x80 + o), (uint8_t)(e[3] & 15));
            }
        }
    }
    md_panw(c);
    md_fnw(c);
    md_ym(0, 0x28, (uint8_t)(c->kc | 0xF0));
}
static RAM_CODE void md_env2(struct md_ch *c)
{
    const uint8_t *in = md_inst + c->inst * 11, *e = md_env + c->env * 7;
    for (uint16_t k = 0; k < 4; k++)
        if (in[1] >> k & 1) {
            uint8_t o = (uint8_t)(md_slotof[k] + c->cc);
            md_ym(c->part, (uint8_t)(0x60 + o), e[4]); md_ym(c->part, (uint8_t)(0x70 + o), e[5]);
        }
}

/* ---- the PCM voices */
static RAM_CODE void md_vkey(uint16_t v, uint8_t smp, uint16_t step, uint8_t lvl)
{
    struct md_voice *x = &md_v[v];
    if (smp >= md_bn) { x->on = 0; return; }
    x->base = FM_WIN5 + md_bank + md_boff[smp];
    x->pos = x->frac = 0;
    x->sint = step >> 12; x->sfrac = (uint16_t)(step << 4);
    x->n = md_blen[smp]; x->ln = md_bloop[smp];
    x->lvl = lvl < MD_LEVELS ? lvl : MD_LEVELS - 1;
    x->on = 1;
}
/* a PCM command (44-47) for voice v; att: the levels added (effects: the song's master volume) */
static RAM_CODE void md_pcmcmd(const volatile uint8_t *p, uint16_t v, uint16_t att)
{
    struct md_voice *x = &md_v[v & 7];
    switch (p[0]) {
    case 0x44: { uint16_t l = (uint16_t)(p[5] + att); md_vkey(v & 7, p[2], (uint16_t)(p[3] << 8 | p[4]), (uint8_t)(l > 255 ? 255 : l)); break; }
    case 0x45: { uint16_t l = (uint16_t)(p[2] + att); x->lvl = l < MD_LEVELS ? (uint8_t)l : MD_LEVELS - 1; break; }
    case 0x46: { uint16_t s = (uint16_t)(p[2] << 8 | p[3]); x->sint = s >> 12; x->sfrac = (uint16_t)(s << 4); break; }
    case 0x47: x->on = 0; break;
    }
}

/* a song tick: the 7 FM / PSG streams, then the PCM stream (8th) */
static RAM_CODE void md_song_tick(void)
{
    for (uint16_t s = 0; s < 8; s++) {
        struct md_ch *c = &md_c[s];
        if (!c->on) continue;
        if (c->wait) { if (--c->wait) continue; }
        struct fm_dec *d = &md_d[s];
        for (uint16_t guard = 0; guard < 256; guard++) {
            if (!d->plen && !fm_next(d)) { c->on = 0; break; }
            const uint8_t *p = d->pend;
            uint8_t op = p[0];
            d->plen = 0;
            if (op < 0x40) { c->wait = (uint16_t)(op + 1); break; }
            if (op == 0x40) { c->wait = (uint16_t)(p[1] | p[2] << 8); break; }
            if (op == 0x42) { c->on = 0; break; }
            if (s == 7) {                                /* the PCM stream */
                if (op >= 0x44 && op <= 0x47 && !md_v[p[1] & 7].sfx) md_pcmcmd(p, p[1], 0);
                continue;
            }
            if (op >= 0x80) {                            /* 1wwddddd: FREQ += d, WAIT ww */
                int16_t dd = (int16_t)(op & 31); if (dd & 16) dd -= 32;
                c->fn = (uint16_t)(c->fn + dd); md_fnw(c);
                uint8_t ww = (op >> 5) & 3;
                if (ww) { c->wait = ww; break; }
                continue;
            }
            switch (op) {
            case 0x50: c->inst = p[1]; c->env = p[2]; c->fn = (uint16_t)((p[3] & 0x3F) << 8 | p[4]); c->pan = p[3] >> 6 & 3;
                       c->vol = p[5]; md_keyon(c); break;
            case 0x51: c->fn = (uint16_t)((p[1] & 0x3F) << 8 | p[2]); md_fnw(c); break;
            case 0x52: c->vol = p[1]; md_carriers(c); break;
            case 0x53: md_ym(0, 0x28, c->kc); break;
            case 0x54: md_env2(c); break;
            case 0x55: c->pan = p[1]; md_panw(c); break;
            case 0x56: for (uint16_t i = 0; i <= p[1]; i++) md_psg(p[2 + i]); break;
            case 0x57: c->vol = (uint8_t)(c->vol + p[1]); md_carriers(c); break;
            case 0x58: c->fn = (uint16_t)(c->fn + (int8_t)p[1]); md_fnw(c); break;
            case 0x59: case 0x5A: case 0x5B: md_ym(0, 0x28, c->kc); c->wait = (uint16_t)(op - 0x58); break;
            case 0x5C: case 0x5D: case 0x5E: case 0x5F: c->vol = (uint8_t)(c->vol + p[1]); md_carriers(c); c->wait = (uint16_t)(op - 0x5B); break;
            default: c->on = 0; break;
            }
            if (c->wait || !c->on) break;
        }
    }
}

/* an effects tick: every voice an effect owns (its stream: uncompressed, window 5) */
static RAM_CODE void md_sfx_tick(void)
{
    for (uint16_t v = 0; v < 8; v++) {
        struct md_voice *x = &md_v[v];
        if (!x->sfx) continue;
        if (x->sw) { if (--x->sw) continue; }
        for (uint16_t guard = 0; guard < 64; guard++) {
            const volatile uint8_t *p = fm_p(x->sp);
            uint8_t op = p[0];
            if (op < 0x40) { x->sw = (uint16_t)(op + 1); x->sp++; break; }
            if (op == 0x40) { x->sw = (uint16_t)(p[1] | p[2] << 8); x->sp += 3; break; }
            if (op >= 0x44 && op <= 0x47) {
                md_pcmcmd(p, v, md_sfxatt);
                x->sp += op == 0x44 ? 6 : op == 0x45 ? 3 : op == 0x46 ? 4 : 2;
                continue;
            }
            x->sfx = 0; x->on = 0; break;                 /* (END or anything else: the effect is over) */
        }
    }
}

/* the ticks: the next one's entry (ACC as the Z80 driver: AINC a sample, a tick when >= TK) */
static RAM_CODE uint32_t md_next_tick(uint32_t *acc)
{
    uint32_t r = md_tk - *acc + md_ainc - 1;        /* (ceil: (TK - ACC) / AINC, divu 32 / 16 -> 16) */
    uint32_t q = r;
    __asm__("divu.w %1,%0" : "+d"(q) : "d"((uint16_t)md_ainc));
    uint16_t j = (uint16_t)q;
    *acc = *acc + mulu16(j, (uint16_t)md_ainc) - md_tk;
    return j;
}

/* mix n (<= MD_CHUNK) samples into md_acc */
static RAM_CODE __attribute__((optimize("O2"))) void md_mix(uint16_t n)
{
    int16_t *a = md_acc;
    {   /* (cleared in asm: GCC would call memset) */
        int16_t *c = a; uint16_t k = n;
        __asm__ volatile("subq.w #1,%1\n1: clr.w (%0)+\n dbra %1,1b" : "+a"(c), "+d"(k) : : "cc", "memory");
    }
    for (uint16_t v = 0; v < 8; v++) {
        struct md_voice *x = &md_v[v];
        if (!x->on) continue;
#ifdef MDPROF
        md_vs += n;
#endif
        uint32_t pos = x->pos;
        uint16_t frac = x->frac, cnt = n;
        __asm__ volatile(
            "   moveq #0,%%d0\n"
            "   subq.w #1,%[n]\n"
            "1: move.b (%[s],%[pos].l),%%d0\n"
            "   move.b (%[t],%%d0.w),%%d1\n"
            "   ext.w %%d1\n"
            "   add.w %%d1,(%[a])+\n"
            "   add.w %[sf],%[fr]\n"
            "   addx.w %[si],%[pos]\n"
            "   dbra %[n],1b\n"
            : [pos] "+d"(pos), [fr] "+d"(frac), [a] "+a"(a), [n] "+d"(cnt)
            : [s] "a"(x->base), [t] "a"(md_vt[x->lvl]), [sf] "d"(x->sfrac), [si] "d"(x->sint)
            : "d0", "d1", "cc", "memory");
        a = md_acc;
        x->frac = frac;
        uint16_t p = (uint16_t)pos;
        if (p >= x->n) {
            if (x->ln) { while (p >= x->n) p = (uint16_t)(p - x->ln); }
            else x->on = 0;
        }
        x->pos = p;
    }
}

/* the Z80's bus (RAM code: the ROM's z80_req would be fetched from the cartridge the SH-2s keep busy) */
static inline __attribute__((always_inline)) void md_req(void)
{
    Z80_BUSREQ = 0x100;
    for (uint16_t i = 0; i < 2000 && (Z80_BUSREQ & 0x100); i++)
        ;
}
/* the ring topped up (md_idle, interrupts on): ticks, mixing, the entries - the bus held per 8 entries.  An
   entry: sample (1-255, written last: 0 = not yet), cmd (0 none - the Z80 clears a played one, so a plain
   entry is one byte), reg, val */
static RAM_CODE __attribute__((optimize("O2"))) void md_work(void)
{
    if (!md_on || !md_drv) return;
    uint8_t ld_v0 = VDP_VCNT;
    uint16_t bk = BANKREG;
    BANKREG = 2;
    md_req();
    uint16_t drd = (uint16_t)(Z80_RAM[MD_MB + 2] | Z80_RAM[MD_MB + 3] << 8);
    if (Z80_RAM[MD_MB + 4] & 1) { md_dry++; Z80_RAM[MD_MB + 4] = 0; }
    Z80_BUSREQ = 0;
    uint16_t rd = (uint16_t)(((drd - MD_RING) >> 2) & (MD_NENT - 1));
    uint16_t used = (uint16_t)((md_w - rd) & (MD_NENT - 1));
    uint16_t want = used < MD_FILL ? MD_FILL - used : 0;
#ifdef MDPROF
    if ((md_calls & 255) == 128) {     /* (test: 20000-cycle dbra loop timed with interrupts on, then off) */
        uint8_t v0 = VDP_VCNT; uint16_t k = 1999;
        __asm__ volatile("1: dbra %0,1b" : "+d"(k));
        md_bench[0] = (uint8_t)(VDP_VCNT - v0);
        __asm__ volatile("move.w #0x2700,%sr");
        v0 = VDP_VCNT; k = 1999;
        __asm__ volatile("1: dbra %0,1b" : "+d"(k));
        md_bench[1] = (uint8_t)(VDP_VCNT - v0);
        __asm__ volatile("move.w #0x2300,%sr");
    }
    md_calls++; md_wsum += want;
    uint16_t pf0 = fm_seq_now;
#endif
    while (want) {
        if (md_playing && md_t == md_mnext) { md_song_tick(); md_mnext += md_next_tick(&md_macc); }
        if (md_t == md_snext) { md_sfx_tick(); md_snext += md_next_tick(&md_sacc); }
        uint16_t n = want < MD_CHUNK ? want : MD_CHUNK;
        if (md_playing && md_mnext - md_t < n) n = (uint16_t)(md_mnext - md_t);
        if (md_snext - md_t < n) n = (uint16_t)(md_snext - md_t);
        if (MD_NENT - md_w < n) n = MD_NENT - md_w;             /* (no wrap inside a piece) */
#ifdef MDPROF
        uint8_t pv0 = VDP_VCNT;
#endif
        md_mix(n);
#ifdef MDPROF
        uint8_t pv1 = VDP_VCNT;
        md_tmix += (uint8_t)(pv1 - pv0);
#endif
        const int16_t *a = md_acc;
        volatile uint8_t *z = Z80_RAM + MD_RING + 4 * md_w;
        for (uint16_t i = 0; i < n; ) {
            if (md_qt == md_qh) {
                /* (no register write waiting: samples only, through the clip table - ~45 cycles an entry, 8
                   entries: the bus held ~60 us, well under the Z80's slack - it loses no period) */
                uint16_t k = n - i < 8 ? n - i : 8, c = k;
                md_req();
                __asm__ volatile(
                    "   subq.w #1,%[c]\n"
                    "1: move.w (%[a])+,%%d0\n"
                    "   move.b (%[t],%%d0.w),(%[z])\n"
                    "   addq.l #4,%[z]\n"
                    "   dbra %[c],1b\n"
                    : [a] "+a"(a), [z] "+a"(z), [c] "+d"(c)
                    : [t] "a"(md_clip + 1024)
                    : "d0", "cc", "memory");
                Z80_BUSREQ = 0;
                i += k;
                continue;
            }
            uint16_t k = n - i < 4 ? n - i : 4;          /* (with register writes: 4 entries a hold) */
            md_req();
            for (uint16_t j = 0; j < k; j++, z += 4) {
                int16_t x = *a++;
                if (md_qt != md_qh) {
                    const uint8_t *q = md_q[md_qt];
                    z[1] = q[0]; z[2] = q[1]; z[3] = q[2];
                    md_qt = (md_qt + 1) & (MD_FMQ - 1);
                }
                z[0] = md_clip[1024 + x];
            }
            Z80_BUSREQ = 0;
            i += k;
        }
#ifdef MDPROF
        md_twr += (uint8_t)(VDP_VCNT - pv1);
#endif
        md_w = (uint16_t)((md_w + n) & (MD_NENT - 1));
        md_t += n;
        want -= n;
        md_fed += n;
    }
    BANKREG = bk;
    md_lines += (uint8_t)(VDP_VCNT - ld_v0);
#ifdef MDPROF
    md_frs += (uint16_t)((fm_seq_now - pf0) & 63);
#endif
}

/* everything quiet: the FM keys off, the PSG silent, the music's PCM voices off (effects play on) */
static RAM_CODE void md_quiet(void)
{
    md_playing = 0;
    for (uint16_t k = 0; k < 6; k++) md_ym(0, 0x28, (uint8_t)(k < 3 ? k : (k - 3) | 4));
    md_psg(0x9F); md_psg(0xBF); md_psg(0xDF); md_psg(0xFF);
    for (uint16_t v = 0; v < 8; v++) if (!md_v[v].sfx) md_v[v].on = 0;
}

/* 0x9Ann: a song (interrupts off; the MD page in window 5) */
static RAM_CODE void md_song(uint16_t song)
{
#ifdef MDPROF
    md_nsong++; md_lastsong = song;
#endif
    md_quiet();
    if (song == 0xFF) return;
    uint16_t bk = BANKREG;
    BANKREG = 2;
    if (song >= fm_rd16(md_dir + 4)) { BANKREG = bk; return; }
    uint32_t e = fm_rd32(md_dir + 32 + 4UL * song);
    uint32_t s = e & 0xFFFFFF;
    if (!e || fm_rd32(s) != 0x464D5332UL) { BANKREG = bk; return; }      /* 'FMS2' */
    uint16_t ni = fm_rd16(s + 4), ne = fm_rd16(s + 6);
    if (ni > 32) ni = 32;
    if (ne > 146) ne = 146;
    uint32_t oi = s + fm_rd32(s + 12), oe = s + fm_rd32(s + 16);
    for (uint16_t i = 0; i < ni * 11; i++) md_inst[i] = fm_p(oi)[i];
    for (uint16_t i = 0; i < ne * 7; i++) md_env[i] = fm_p(oe)[i];
    fm_blob = s + fm_rd32(s + 20);
    for (uint16_t k = 0; k < 8; k++) {
        struct fm_dec *d = &md_d[k];
        uint16_t st = fm_rd16(s + 24 + 2 * k), lp = fm_rd16(s + 40 + 2 * k);
        d->pos = fm_blob + st;
        d->end = 0xFFFFFFFFUL;
        d->loop = lp == 0xFFFF ? 0xFFFFFFFFUL : fm_blob + lp;
        d->depth = d->done = d->plen = 0;
        struct md_ch *c = &md_c[k];
        c->wait = 0; c->fn = 0; c->vol = 0; c->inst = c->env = 0; c->pan = 3; c->on = 1;
        c->part = k >= 3 && k < 6; c->cc = (uint8_t)(k < 3 ? k : k < 6 ? k - 3 : 0);
        c->kc = (uint8_t)(k < 3 ? k : k < 6 ? (k - 3) | 4 : 0);
    }
    md_sfxatt = fm_p(fm_rd32(md_dir + 24))[song];
    BANKREG = bk;
    md_mnext = md_t;                         /* (tick 0: the next entry written) */
    md_macc = 0;
    md_playing = 1;
}

/* 0x9vxx: effect xx on voice v */
static RAM_CODE void md_sfx_cmd(uint16_t cmd)
{
    if (!md_on) return;
    uint16_t v = (cmd >> 8) & 7, id = cmd & 0xFF;
    uint16_t bk = BANKREG;
    BANKREG = 2;
    if (id < fm_rd16(md_sfx)) {
        uint16_t o = fm_rd16(md_sfx + 4 + 2 * id);
        if (o) {
#ifdef MDPROF
            md_nsfx++;
#endif
            struct md_voice *x = &md_v[v];
            x->sfx = 1; x->sp = md_sfx + o; x->sw = 0; x->on = 0;
        }
    }
    BANKREG = bk;
}

static const uint8_t md_gain[MD_LEVELS] = { 0, 215, 181, 152, 128, 108, 91, 76, 64, 54, 46, 38, 32, 27, 23, 19,
                                            16, 14, 11, 10, 8, 7, 6, 5, 4, 3, 3, 2, 2, 2, 1, 1 };   /* (0: 256) */
/* SOUND MD on (0x9B05 / 07): the MD page, the Z80 player, the tables */
static RAM_CODE void md_enable(void)
{
    fm_stop();
    fm_mode = 0;
    fm_step = 0;
    if (!fm_firstbank) return;
    uint16_t bk = BANKREG;
    BANKREG = 2;
    fm_map(fm_firstbank);
    uint16_t pg = fm_rd16(fm_tail + 6);
    uint32_t dir = (uint32_t)fm_rd16(fm_tail + 14) << 4;
    if (!pg || !dir) { fm_map(5); BANKREG = bk; return; }
    fm_map(pg);
    if (fm_rd32(dir) != 0x4D444431UL) { fm_map(5); BANKREG = bk; return; }    /* 'MDD1' */
    md_page = pg;
    md_dir = dir;
    md_units = fm_rd16(dir + 6);
    md_att = fm_rd16(dir + 14);
    md_bank = fm_rd32(dir + 16);
    md_sfx = fm_rd32(dir + 20);
    md_bn = fm_rd16(md_bank);
    if (md_bn > 64) md_bn = 64;
    for (uint16_t i = 0; i < md_bn; i++) {
        uint32_t q = md_bank + 4 + 8UL * i;
        md_boff[i] = fm_rd32(q); md_blen[i] = fm_rd16(q + 4); md_bloop[i] = fm_rd16(q + 6);
    }
    for (int16_t x = -1024; x < 1024; x++)
        md_clip[x + 1024] = (uint8_t)(x < -127 ? 1 : x > 127 ? 255 : x + 128);
    for (uint16_t l = 0; l < MD_LEVELS; l++) {          /* VOL[l][b] = b x gain >> 8 (asr) */
        int16_t g = l ? md_gain[l] : 256;
        for (uint16_t b = 0; b < 256; b++) {
            int16_t sv = (int16_t)(int8_t)b;
            int32_t m = sv;
            __asm__("muls.w %1,%0" : "+d"(m) : "d"(g));
            md_vt[l][b] = (int8_t)(m >> 8);
        }
    }
    /* the player: Z80 held in reset, the chips silent (fm_hush), the code, the ring cleared, the timer value */
    uint32_t o = fm_rd32(dir + 8);
    uint16_t n = fm_rd16(dir + 12);
    fm_hush();
    Z80_BUSREQ = 0x100;
    Z80_RESETL = 0x100;
    for (uint16_t i = 0; i < 2000 && (Z80_BUSREQ & 0x100); i++)
        ;
    for (uint16_t i = 0; i < n; i++) Z80_RAM[i] = fm_p(o)[i];
    for (uint16_t i = 0; i < MD_NENT * 4; i++) Z80_RAM[MD_RING + i] = 0;
    for (uint16_t i = MD_MB; i < MD_MB + 0x20; i++) Z80_RAM[i] = 0;
    uint16_t tav = (uint16_t)(1024 - md_units);
    Z80_RAM[MD_MB + 0] = (uint8_t)tav; Z80_RAM[MD_MB + 1] = (uint8_t)(tav >> 8);
    Z80_RESETL = 0;
    for (volatile uint16_t i = 0; i < 64; i++)
        ;
    Z80_RESETL = 0x100;
    z80_rel();
    BANKREG = bk;
    for (uint16_t r = 0; r < 256; r++) md_sh[0][r] = md_sh[1][r] = 0x100;
    md_qh = md_qt = 0;
    for (uint16_t v = 0; v < 8; v++) { md_v[v].on = 0; md_v[v].sfx = 0; }
    md_w = 0;
    md_t = md_snext = 0;
    md_sacc = 0;
    md_tk = md_pal ? 168899UL : 170455UL;
    md_ainc = 256UL * md_units;
    md_playing = 0;
    md_drv = 1;
    md_on = 1;
#ifdef MDPROF
    {   /* (test: the 68000's speed - 2000 x dbra = 20000 cycles = 41 lines at full speed; and a RAM loop) */
        uint8_t v0 = VDP_VCNT; uint16_t k = 1999;
        __asm__ volatile("1: dbra %0,1b" : "+d"(k));
        md_bench[0] = (uint8_t)(VDP_VCNT - v0);
        v0 = VDP_VCNT;
        int16_t *a = md_acc; uint16_t c = 2047;      /* 2048 x (clr.w (a)+ 12 + dbra 10) = 45056 cycles = 92 lines */
        __asm__ volatile("1: clr.w (%0)\n dbra %1,1b" : "+a"(a), "+d"(c) : : "memory");
        md_bench[1] = (uint8_t)(VDP_VCNT - v0);
    }
#endif
}
static RAM_CODE void md_disable(void)
{
    if (!md_on) return;
    md_on = 0;
    md_playing = 0;
    md_drv = 0;
    fm_hush();                                       /* (Z80 in reset, the chips silent, the DAC off) */
    fm_map(5);
}
#pragma GCC pop_options
