/* (I24) SOUND FM - the 68000's part (included by main.c).
 *
 * The music's FM / PSG half plays on the Z80 (z80/fmdrv.asm); the drums, hi-hats and ambience stay on the
 * 32X (the S-DSP voices of those samples, mixed by the slave with the sound effects).  The song data sits
 * in the FM pages: ROM banks that replace window 5 (cart $280000-$2FFFFF) while SOUND FM is on - each an
 * exact copy of bank 5 except its tail, where bank 5 has the SNES-only music samples (FM mode never
 * mixes those) and an FM page has the FM directory, the Z80 driver and songs (tools/audio/fm/fmbuild.py).
 * The Z80 never touches the 68000 bus: this code loads the driver and the song's tables, LZ-expands the
 * 7 command streams and keeps the Z80's rings full (every vblank), and starts a song at the vblank the
 * SH-2 names, so that the Z80's tick 0 sounds with the 32X's (the slave knows when its song starts).
 *
 * SH-2 commands: 0x9B0f FM mode (f bit 0 on, bit 1 mono), 0x98ss seq (6 bits) + offset bits 8-9,
 * 0x99oo offset bits 0-7 (32 us units after that vblank), 0x9Ann song (0xFF: stop) - sent in that order.
 */
#pragma GCC push_options
#pragma GCC optimize ("Os")            /* (the first 256 KB of ROM are full) */
#define Z80_RAM    ((volatile uint8_t *)0xA00000)
#define Z80_BUSREQ (*(volatile uint16_t *)0xA11100)
#define Z80_RESETL (*(volatile uint16_t *)0xA11200)
#define FM_WIN5    0x980000UL        /* window 5 through the 1 MB bank window, bank register 2 */
#define MB 0x1F00
#define NSTR 7
#define FM_DEPTH 8                   /* CALL nesting (tools/audio/fm/fmpack.py CALL_DEPTH) */

struct fm_dec {                      /* one stream's LZ decoder (window-5 offsets) */
    uint32_t pos, end, loop;
    uint32_t sp[FM_DEPTH], se[FM_DEPTH];
    uint8_t depth, done, plen, pend[13];   /* pend: a decoded command that did not fit the ring yet */
};
static struct fm_dec fm_d[NSTR];
static uint16_t fm_mode, fm_flags, fm_drv, fm_playing, fm_song = 0xFFFF;
static uint16_t fm_bank, fm_seq, fm_off, fm_start, fm_late;
static uint32_t fm_tail, fm_tk;
static uint16_t fm_firstbank;
static uint16_t fm_wr[NSTR];          /* ring write offsets (mirrors of the Z80's) */
static uint32_t fm_feeds, fm_starved;
static uint32_t fm_blob;              /* the song's stream blob (window-5 offset; CALL offsets are relative) */
static uint16_t fm_seq_now;           /* the 68000's vblank number (the C_PAD sample number) - md_vint */
static inline uint16_t fm_cur_seq(void) { return fm_seq_now; }
static void fm_go(void);
static inline uint16_t fm_lines(void)               /* lines since the last vblank began */
{
    uint8_t v = VDP_VCNT;
    return v >= 224 ? v - 224 : v + (md_pal ? 313 - 224 : 262 - 224);
}

/* (volatile: byte reads only - GCC merged p[1] | p[2] << 8 into a word read + rorw, an odd address on the
   68000: PicoDrive read the aligned word instead - I24 round 2) */
static inline const volatile uint8_t *fm_p(uint32_t off) { return (const volatile uint8_t *)(FM_WIN5 + off); }
static inline uint32_t fm_rd32(uint32_t off)
{
    const volatile uint8_t *p = fm_p(off);
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static inline uint16_t fm_rd16(uint32_t off) { const volatile uint8_t *p = fm_p(off); return (uint16_t)(p[0] << 8 | p[1]); }

static inline void z80_req(void)
{
    Z80_BUSREQ = 0x100;
    for (uint16_t i = 0; i < 2000 && (Z80_BUSREQ & 0x100); i++)
        ;
}
static inline void z80_rel(void) { Z80_BUSREQ = 0; }

/* window 5 -> bank b (SSF2 register $A130FB; RV = 1 meanwhile: mapper_w) */
static RAM_CODE void fm_map(uint16_t b)
{
    if (b == fm_bank) return;
    mapper_w(0xA130FB, (uint8_t)b);
    fm_bank = b;
}

/* the command length of op at p (fmpack.py cmd_len) */
static RAM_CODE uint16_t fm_len(const volatile uint8_t *p)
{
    uint8_t op = p[0];
    if (op < 0x40 || op >= 0x80) return 1;
    if (op >= 0x60) return 3;         /* CALL3 */
    switch (op) {
    case 0x40: return 3;
    case 0x41: return 4;
    case 0x50: return 6;
    case 0x51: return 3;
    case 0x52: case 0x55: case 0x57: case 0x58: case 0x5C: case 0x5D: case 0x5E: case 0x5F: return 2;
    case 0x56: return (uint16_t)(p[1] + 3);
    default: return 1;                /* 0x42 0x43 0x53 0x54 0x59-0x5B */
    }
}

/* the next command of stream s into d->pend (CALL / return / LOOP followed); 0 = the stream ended */
static RAM_CODE uint16_t fm_next(struct fm_dec *d)
{
    for (uint16_t guard = 0; guard < 4 * FM_DEPTH + 4; guard++) {
        if (d->done) return 0;
        while (d->depth && d->pos >= d->end) {
            d->depth--;
            d->pos = d->sp[d->depth];
            d->end = d->se[d->depth];
        }
        const volatile uint8_t *p = fm_p(d->pos);
        uint8_t op = p[0];
        if (op == 0x41 || (op >= 0x60 && op < 0x80)) {   /* CALL o16 len8 / CALL3 (len op - 0x5D): blob offsets */
            if (d->depth >= FM_DEPTH) { d->done = 1; return 0; }
            uint16_t n = op == 0x41 ? 4 : 3;
            d->sp[d->depth] = d->pos + n;
            d->se[d->depth] = d->end;
            d->depth++;
            uint32_t o = fm_blob + (uint16_t)(p[1] | p[2] << 8);
            d->pos = o;
            d->end = o + (op == 0x41 ? p[3] : op - 0x5D);
            continue;
        }
        if (op == 0x43) {             /* LOOP: back to the loop point */
            if (d->loop == 0xFFFFFFFFUL) { d->done = 1; return 0; }
            d->pos = d->loop;
            d->end = 0xFFFFFFFFUL;
            d->depth = 0;
            continue;
        }
        uint16_t n = fm_len(p);
        for (uint16_t i = 0; i < n; i++) d->pend[i] = p[i];
        d->plen = (uint8_t)n;
        d->pos += n;
        if (op == 0x42) d->done = 1;  /* END: passed on, the Z80 stops this stream */
        return n;
    }
    d->done = 1;
    return 0;
}

/* the rings topped up: whole commands, at most max bytes a stream and budget in all.  The commands are
   decoded into fm_stage first; the Z80's bus is held only to read its ring positions and for each copy of
   at most 64 bytes (~0.2 ms: the Z80 polls its 1 ms timer - a longer hold would lose a period) */
static uint8_t fm_stage[64 + 16];
static RAM_CODE uint16_t fm_feed(uint16_t budget, uint16_t max)
{
    uint16_t rd[NSTR], put = 0;
    z80_req();
    for (uint16_t s = 0; s < NSTR; s++)
        rd[s] = (uint16_t)(Z80_RAM[MB + 0x20 + 2 * s] | Z80_RAM[MB + 0x21 + 2 * s] << 8);
    z80_rel();
    for (uint16_t s = 0; s < NSTR && put < budget; s++) {
        struct fm_dec *d = &fm_d[s];
        uint16_t wr = fm_wr[s], room = (uint16_t)((rd[s] - wr - 1) & 511), done = 0;
        if (room > max) room = max;
        for (;;) {
            uint16_t n = 0;
            while (n < 64) {                         /* (stage <= 64 bytes of whole commands) */
                if (!d->plen && !fm_next(d)) break;
                if (done + n + d->plen > room || put + n + d->plen > budget || n + d->plen > 64 + 16) break;
                for (uint16_t i = 0; i < d->plen; i++) fm_stage[n + i] = d->pend[i];
                n += d->plen;
                d->plen = 0;
            }
            if (!n) break;
            volatile uint8_t *ring = Z80_RAM + 0x1000 + s * 512;
            z80_req();
            for (uint16_t i = 0; i < n; i++) { ring[wr] = fm_stage[i]; wr = (wr + 1) & 511; }
            Z80_RAM[MB + 0x10 + 2 * s] = (uint8_t)wr;
            Z80_RAM[MB + 0x11 + 2 * s] = (uint8_t)(wr >> 8);
            z80_rel();
            done += n;
            put += n;
        }
        fm_wr[s] = wr;
    }
    return put;
}

/* the Z80 driver from the FM page (z80 held in reset meanwhile) */
static RAM_CODE void fm_load_driver(void)
{
    uint32_t o = fm_rd32(fm_tail + 8), n = fm_rd16(fm_tail + 12);
    Z80_BUSREQ = 0x100;
    Z80_RESETL = 0x100;
    for (uint16_t i = 0; i < 2000 && (Z80_BUSREQ & 0x100); i++)
        ;
    for (uint16_t i = 0; i < n; i++) Z80_RAM[i] = fm_p(o)[i];
    for (uint16_t i = MB; i < MB + 0x40; i++) Z80_RAM[i] = 0;   /* (the mailbox; the driver clears the rest) */
    Z80_RESETL = 0;
    for (volatile uint16_t i = 0; i < 64; i++)
        ;
    Z80_RESETL = 0x100;
    z80_rel();
    for (volatile uint16_t i = 0; i < 2000; i++)          /* (its init: the timer, silence) */
        ;
    fm_drv = 1;
}

static RAM_CODE void fm_stop(void)
{
    fm_start = 0;
    if (!fm_drv) return;
    z80_req();
    Z80_RAM[MB + 0] = 2;
    z80_rel();
    for (uint16_t t = 0; t < 4000; t++) {            /* (until it has stopped: rings / tables are reused) */
        z80_req();
        uint8_t c = Z80_RAM[MB + 0], st = Z80_RAM[MB + 0x0B];
        z80_rel();
        if (!c && !(st & 1)) break;
        for (volatile uint16_t i = 0; i < 20; i++)
            ;
    }
    fm_playing = 0;
}

/* the song set up (md_idle, interrupts off - in two steps, each well under a frame: the vblanks keep coming,
   so fm_seq_now stays right; 0x9Ann only stops the old song and asks for it) */
static uint16_t fm_step;
static RAM_CODE void fm_setup(uint16_t song)
{
    fm_song = song;
    if (song == 0xFF || !fm_mode || !fm_firstbank) return;
    /* the directory is in every FM page: map one, find the song's */
    if (fm_bank < fm_firstbank) fm_map(fm_firstbank);
    uint16_t bk = BANKREG;
    BANKREG = 2;
    if (fm_rd32(fm_tail) != 0x464D4431UL || song >= fm_rd16(fm_tail + 4)) { BANKREG = bk; return; }   /* 'FMD1' */
    uint32_t e = fm_rd32(fm_tail + 16 + 4UL * song);
    if (!e) { BANKREG = bk; return; }
    fm_map((uint16_t)(e >> 24));
    BANKREG = 2;
    if (!fm_drv) fm_load_driver();
    uint32_t s = e & 0xFFFFFF;                       /* the song block (window-5 offset) */
    if (fm_rd32(s) != 0x464D5331UL) { BANKREG = bk; return; }   /* 'FMS1' */
    uint16_t ni = fm_rd16(s + 4), ne = fm_rd16(s + 6);
    uint32_t oi = s + fm_rd32(s + 12), oe = s + fm_rd32(s + 16);
    fm_blob = s + fm_rd32(s + 20);
    z80_req();
    for (uint16_t i = 0; i < ni * 11; i++) Z80_RAM[0x0C00 + i] = fm_p(oi)[i];
    for (uint16_t i = 0; i < ne * 7; i++) Z80_RAM[0x0800 + i] = fm_p(oe)[i];
    for (uint16_t k = 0; k < NSTR; k++) {
        struct fm_dec *d = &fm_d[k];
        uint16_t st = fm_rd16(s + 24 + 2 * k), lp = fm_rd16(s + 38 + 2 * k);
        d->pos = fm_blob + st;
        d->end = 0xFFFFFFFFUL;
        d->loop = lp == 0xFFFF ? 0xFFFFFFFFUL : fm_blob + lp;
        d->depth = d->done = d->plen = 0;
        fm_wr[k] = 0;
        Z80_RAM[MB + 0x10 + 2 * k] = Z80_RAM[MB + 0x11 + 2 * k] = 0;
        Z80_RAM[MB + 0x20 + 2 * k] = Z80_RAM[MB + 0x21 + 2 * k] = 0;
    }
    Z80_RAM[MB + 1] = (uint8_t)(fm_flags & 2);
    Z80_RAM[MB + 7] = (uint8_t)fm_tk; Z80_RAM[MB + 8] = (uint8_t)(fm_tk >> 8); Z80_RAM[MB + 9] = (uint8_t)(fm_tk >> 16);
    Z80_RAM[MB + 0x0E] = 0;
    z80_rel();
    BANKREG = bk;
    fm_step = 2;
}
static RAM_CODE void fm_prefill(void)
{
    /* (a first part of each stream - the vblanks bring the rest, 160 bytes each) */
    uint16_t bk = BANKREG;
    BANKREG = 2;
    fm_feed(NSTR * 40, 40);
    BANKREG = bk;
    fm_step = 0;
    fm_start = 1;
    /* "at once" (the SH-2 does not know the 68000's vblank numbers yet: offset 1023) */
    if (fm_off == 0x3FF) { fm_off = fm_lines() * 2; fm_late = 1; fm_seq = fm_cur_seq(); fm_go(); return; }
    /* late already (the SH-2's vblank has passed)?  Then at once, with the time it has played */
    uint16_t late = (uint16_t)((fm_cur_seq() - fm_seq) & 63);
    if (late < 32) { fm_late = late + 1; fm_go(); }
}

/* start the prepared song: the Z80's tick 0 at (vblank fm_seq + fm_off x 32 us); fm_late = vblanks since
   that one + 1 */
/* (no libgcc: 16 x 16 -> 32 multiplies only) */
static inline uint32_t mulu16(uint16_t a, uint16_t b)
{
    uint32_t r = a;
    __asm__("mulu.w %1,%0" : "+d"(r) : "d"(b));
    return r;
}
static RAM_CODE void fm_go(void)
{
    /* everything in 32 us steps: the time since the named vblank (the frames since + this one's line: the
       vblank interrupt itself may come late - a command or a set-up step with interrupts off) */
    uint16_t f = fm_late ? fm_late - 1 : 0;
    uint16_t t = (uint16_t)(md_pal ? f * 629 : f * 521 + (f >> 1)) + fm_lines() * 2;   /* (20120 / 16688 us a frame) */
    uint16_t ahead = fm_off >= t ? fm_off - t : 0, past = fm_off >= t ? 0 : t - fm_off;
    uint16_t n = ahead ? ahead : past;
    /* 32 us = 436.4 (NTSC) / 432.4 (PAL) units x 256 (a unit = 144 YM clocks) */
    uint32_t u256 = mulu16(n, md_pal ? 432 : 436) + (mulu16(n, 3) >> 3);
    uint32_t pend = 0;
    int32_t acc;
    if (ahead || !past) acc = (int32_t)fm_tk - (int32_t)u256;      /* (negative: the Z80 waits) */
    else {                                           /* (late: tick 0 and every one due at once, then on time) */
        pend = 1;
        while (u256 >= fm_tk) { u256 -= fm_tk; pend++; }
        acc = (int32_t)u256;
    }
    z80_req();
    Z80_RAM[MB + 2] = (uint8_t)pend; Z80_RAM[MB + 3] = (uint8_t)(pend >> 8);
    Z80_RAM[MB + 4] = (uint8_t)acc; Z80_RAM[MB + 5] = (uint8_t)(acc >> 8); Z80_RAM[MB + 6] = (uint8_t)(acc >> 16);
    Z80_RAM[MB + 0] = 1;
    {   /* (test reads: how this song was started) */
        volatile uint8_t *q = Z80_RAM + MB + 0x30;
        q[0] = (uint8_t)fm_seq; q[1] = (uint8_t)fm_off; q[2] = (uint8_t)(fm_off >> 8); q[3] = (uint8_t)fm_late;
        q[4] = (uint8_t)t; q[5] = (uint8_t)(t >> 8); q[6] = (uint8_t)fm_cur_seq(); q[7] = VDP_VCNT;
    }
    z80_rel();
    fm_start = 0;
    fm_playing = 1;
}

/* md_vint, first thing: the scheduled start; later: the rings topped up */
static inline void fm_vint_start(uint16_t seq_next)
{
    fm_seq_now = seq_next;
#ifdef FMDEBUG
    if (fm_drv) { z80_req(); Z80_RAM[MB + 0x38] = (uint8_t)seq_next; z80_rel(); }   /* (test reads) */
#endif
    if (fm_start && seq_next == fm_seq) { fm_late = 1; fm_go(); }
}
static RAM_CODE void fm_vint_feed(void)
{
    if (!fm_playing) return;
    uint16_t bk = BANKREG;
    BANKREG = 2;
    fm_feeds += fm_feed(160, 511);
    BANKREG = bk;
}

/* SH-2 commands 0x98xx-0x9Bxx (interrupts off) */
static RAM_CODE void fm_command(uint16_t cmd)
{
    uint16_t a = cmd & 0xFF;
    switch (cmd >> 8) {
    case 0x98: fm_seq = a & 63; fm_off = (uint16_t)((fm_off & 0xFF) | (a >> 6) << 8); break;
    case 0x99: fm_off = (uint16_t)((fm_off & 0x300) | a); break;
    case 0x9A: fm_stop(); fm_step = 0; fm_song = a; if (a != 0xFF && fm_mode && fm_firstbank) fm_step = 1; break;
    case 0x9B:
        fm_flags = a;
        fm_tk = md_pal ? 168899UL : 170455UL;
        if (!(a & 1)) {                              /* FM off: the 32X's own music again (bank 5) */
            fm_stop();
            fm_step = 0;
            fm_mode = 0;
            fm_map(5);
        } else {
            fm_mode = 1;
            if (fm_drv) { z80_req(); Z80_RAM[MB + 1] = (uint8_t)(a & 2); z80_rel(); }
        }
        break;
    }
    COMM(C_CMD) = 0;
}

/* md_idle, interrupts off: the next step of a song's set-up */
static RAM_CODE void fm_work(void)
{
    if (fm_step == 1) {
        fm_step = 0;
        fm_setup(fm_song);                           /* (-> fm_step 2 when it went well) */
    } else if (fm_step == 2)
        fm_prefill();
}

/* power-on: the FM pages' place (pack header: first FM bank << 24 | the tail's window-5 offset) */
/* (power-on / reset) the Z80 held in reset and the YM2612 / PSG silent: after a reset in SOUND FM the old
   driver would play its stale rings dry and leave notes sounding (the 68000's bus request, YM at $A04000) */
static RAM_CODE void fm_ym_w(uint16_t part, uint8_t r, uint8_t v)
{
    volatile uint8_t *ym = (volatile uint8_t *)0xA04000;
    for (uint16_t i = 0; i < 1000 && (ym[0] & 0x80); i++)
        ;
    ym[part * 2] = r;
    for (uint16_t i = 0; i < 1000 && (ym[0] & 0x80); i++)
        ;
    ym[part * 2 + 1] = v;
}
static RAM_CODE void fm_hush(void)
{
    Z80_BUSREQ = 0x100;                              /* (the usual order: bus, reset off, the grant) */
    Z80_RESETL = 0x100;
    for (uint16_t i = 0; i < 2000 && (Z80_BUSREQ & 0x100); i++)
        ;
    for (uint16_t ch = 0; ch < 7; ch++) {
        if (ch == 3) continue;
        fm_ym_w(0, 0x28, (uint8_t)ch);               /* key off */
        for (uint16_t sl = 0; sl < 16; sl += 4) {
            fm_ym_w(ch >> 2, (uint8_t)(0x40 + sl + (ch & 3)), 0x7F);   /* TL: silent */
            fm_ym_w(ch >> 2, (uint8_t)(0x80 + sl + (ch & 3)), 0xFF);   /* RR 15 */
        }
    }
    fm_ym_w(0, 0x2B, 0);                             /* DAC off */
    volatile uint8_t *psg = (volatile uint8_t *)0xC00011;
    *psg = 0x9F; *psg = 0xBF; *psg = 0xDF; *psg = 0xFF;
    Z80_RESETL = 0;                                  /* (the Z80 stays in reset) */
    Z80_BUSREQ = 0;
    fm_drv = 0;
}

static RAM_CODE void fm_init(uint32_t info)
{
    fm_firstbank = (uint16_t)(info >> 24);
    fm_tail = info & 0xFFFFFF;
    fm_bank = 5;
    mapper_w(0xA130FB, 5);                           /* (a reset in FM mode left an FM page there) */
    if (fm_firstbank) fm_hush();
}
#pragma GCC pop_options
