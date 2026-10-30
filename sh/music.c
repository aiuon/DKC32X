/* 32X music output (slave SH-2).
 *
 * The slave's DMA channel 1 feeds the PWM L/R FIFO (DREQ1 = PWM timer, one long per sample) from a
 * double buffer in SDRAM.  Each time a half finishes, the DMA-end interrupt restarts the DMA on the
 * other half and refills the finished one with musplay (engine ticks + mixer).  It runs whenever it
 * is needed, independent of the renderer.  The master requests songs through music_req (uncached).
 *
 * PicoDrive only clocks the PWM timer (and DREQ1) while a CPU has the PWM interrupt enabled, so the
 * slave enables it in the 32X mask and blocks it with SR (level 6); the DMAC interrupt is level 9 (the slave's H / V interrupts preempt it).
 */
#include <stdint.h>
#include "musplay.h"
#include "music.h"

#define REG8(a)   (*(volatile uint8_t  *)(a))
#define REG16(a)  (*(volatile uint16_t *)(a))
#define REG32(a)  (*(volatile uint32_t *)(a))
#define UNCACHED(p) ((__typeof__(p))((uint32_t)(p) | 0x20000000))

#define PWM_CTRL  0x20004030
#define PWM_CYC   0x20004032
#define PWM_LR    0x20004034
#define SYS_INTMASK 0x20004001

#define SAR1    0xFFFFFF90
#define DAR1    0xFFFFFF94
#define TCR1    0xFFFFFF98
#define CHCR1   0xFFFFFF9C
#define VCRDMA1 0xFFFFFFA8
#define DRCR1   0xFFFFFE72
#define DMAOR   0xFFFFFFB0
#define IPRA    0xFFFFFEE2

/* SH-2 clock: NTSC 23.011361 MHz, PAL 22.801467 MHz (3 x the 68000's); 32X VDP bit 15 = 1 on NTSC */
#define SH2_HZ  ((*(volatile uint16_t *)0x20004100 & 0x8000) ? 23011361u : 22801467u)
static uint32_t cycle_v;
#define CYCLE   cycle_v
#define HALF    128
#define ECHO_FRAMES ((4 * 512 * MUS_RATE + 31999) / 32000)

/* CHCR: dest fixed, source increment, long, DREQ (external) edge/active-high, IE, DE */
#define CHCR_GO 0x18E5

volatile uint32_t music_req;           /* master -> slave: seq << 9 | light << 8 | song (0xFF = silence) */
volatile uint32_t music_ack;           /* slave -> master: last seq handled */
volatile uint32_t music_light;         /* master -> slave: 1 = voices at half rate (adaptive) */
volatile uint32_t music_ticks;         /* FRT ticks spent in the last refill (for the HUD) */
volatile uint32_t music_refills;
volatile uint32_t music_halfs, music_skips, music_maxdt, music_stat_rst, music_lates;
#ifdef MUSHIST
volatile uint32_t music_hist[16], music_hhalf[16];   /* (test) refill time / buffer time in 1/8, full and half rate */
#endif   /* (I18c, test reads) refills over 70 % / 100 % of a buffer, longest */

static uint32_t pwm_buf[2][HALF] __attribute__((aligned(16)));
/* sound effect commands, master -> slave (uncached; one writer each side) */
#define SFXQ 16
static volatile uint16_t sfx_q[SFXQ];
static volatile uint32_t sfx_head;
static uint32_t sfx_tail;

void music_sfx(uint32_t cmd, uint32_t param)
{
    uint32_t h = *UNCACHED(&sfx_head);
    UNCACHED(&sfx_q[0])[h & (SFXQ - 1)] = (uint16_t)(cmd << 8 | (param & 0xFF));
    *UNCACHED(&sfx_head) = h + 1;
}
#ifdef FAKESMP
int16_t fake_smp[64];
#endif
static musplay_t mp;
static uint32_t cur_seq, half;
static int8_t *echo_mem;
static const uint8_t *blob;

/* overload guard: a refill must finish well within the buffer's playing time (HALF samples) */
#define BUF_TICKS ((uint32_t)HALF * (SH2_HZ / 8) / MUS_RATE)  /* FRT ticks (SH-2 clock / 8) */
static uint32_t force_half, skip_next;
static void refill(uint32_t *dst)
{
    uint32_t req = *UNCACHED(&music_req);
    if ((req >> 9) != cur_seq) {
        cur_seq = req >> 9;
        mp_start(&mp, blob, req & 0xFF, echo_mem, ECHO_FRAMES, MUS_RATE);
        mp.dsp.half = (req >> 8) & 1;
        /* light mode = comp levels: two-way cache + on-chip RAM.  Mix buffers in the free top of the
           slave's on-chip RAM (the comp renderer uses the first 1.5 KB) */
        /* (samples always read cached: uncached reads cost ~29 cycles each on MiSTer - the mixer
           then could not keep up and the slave never left the music interrupt) */
        mp.dsp.smp_or = 0;
        mp.dsp.pwm_center = CYCLE / 2;
        *UNCACHED(&music_ack) = cur_seq;
    }
    mp.dsp.half = ((req >> 8) & 1) | *UNCACHED(&music_light) | (force_half != 0);
    if (force_half) force_half--;
    {
        uint32_t h = *UNCACHED(&sfx_head);
        if (h - sfx_tail > SFXQ) sfx_tail = h - SFXQ;      /* (overrun: drop the oldest) */
        for (; sfx_tail != h; sfx_tail++) {
            uint32_t e = UNCACHED(&sfx_q[0])[sfx_tail & (SFXQ - 1)];
            mp_cmd(&mp, e >> 8, e & 0xFF);
        }
    }
    /* only while this CPU's cache is in two-way mode (the switch happens on the first frame) */
    mp.dsp.work = (REG8(0xFFFFFE92) & 0x08) ? (int32_t *)0xC0000600 : 0;
#ifdef NOMIX
    for (int i = 0; i < HALF; i++) dst[i] = (CYCLE / 2) * 0x10001u;
#else
    mp_render(&mp, dst, HALF);
#endif
}

static inline uint16_t frt(void)
{
    uint32_t h = REG8(0xFFFFFE12);          /* high byte first: latches the low byte */
    return (uint16_t)(h << 8 | REG8(0xFFFFFE13));
}

static void dma_go(int h)
{
    REG32(CHCR1) = 0;                       /* clear TE (read-then-write 0) */
    REG32(SAR1) = (uint32_t)pwm_buf[h];
    REG32(DAR1) = PWM_LR;
    REG32(TCR1) = HALF;
    REG32(CHCR1) = CHCR_GO;
}

/* called from the DMA-end interrupt stub (start.s) */
/* (I12: SDRAM - the slave runs it for the music, ~5 cycles per instruction from the cartridge) */
__attribute__((section(".sdtext"), aligned(16))) void music_irq(void)
{
    (void)REG32(CHCR1);
    dma_go(half ^ 1);
    uint16_t t0 = frt();
    if (skip_next) {                        /* behind: one buffer of silence instead of a livelock */
        skip_next = 0;
        for (int i = 0; i < HALF; i++) pwm_buf[half][i] = (CYCLE / 2) * 0x10001u;
    } else
        refill(pwm_buf[half]);
    uint16_t dt = (uint16_t)(frt() - t0);
    if (*(volatile uint32_t *)((uint32_t)&music_stat_rst | 0x20000000)) {
        *(volatile uint32_t *)((uint32_t)&music_stat_rst | 0x20000000) = 0;
        music_halfs = music_skips = music_maxdt = music_lates = 0;
#ifdef MUSHIST
        for (int i = 0; i < 16; i++) music_hist[i] = music_hhalf[i] = 0;
#endif
    }
    if (dt > BUF_TICKS * 7 / 10) { force_half = 512; music_halfs++; }     /* ~3 s of half-rate voices */
    /* (I19) one late refill already cost a short gap (the DMA ran dry meanwhile); a buffer of silence
       on top of it doubled the dropout.  Silence only when this refill AND the last one were late
       (behind for good: the slave must get out of the interrupt) */
    {
        static uint32_t late_last;
        uint32_t late = dt > BUF_TICKS;
        music_lates += late;
        if (late && (late_last || dt > 2 * BUF_TICKS)) { skip_next = 1; music_skips++; late = 0; }
        late_last = late;
    }
    if (dt > music_maxdt) music_maxdt = dt;
#ifdef MUSHIST
    {   uint32_t k = dt * 8 / BUF_TICKS; if (k > 15) k = 15;
        if (mp.dsp.half) music_hhalf[k]++; else music_hist[k]++; }
#endif
    music_ticks += dt;
    half ^= 1;
    music_refills++;
}

uint32_t music_echo_bytes(void)
{
    return ECHO_FRAMES * 2;
}

/* slave: set up PWM + DMA.  'echo' is scratch the master never touches while a song plays. */
#ifdef MUSDEBUG
volatile uint32_t music_bench;
uint32_t mixv(int32_t *acc, const int16_t *pcm, uint32_t pos, const void *a);
#endif

void music_init(const uint8_t *music_blob, int8_t *echo)
{
#ifdef MUSDEBUG
    {   /* micro benchmark: 10 x mixv over 128 frames (one voice), FRT ticks */
        int32_t acc[256];
        uint32_t args[6] = { 0x17000, 128, 1000, 1000, 0x7FFF0000, 0x10000000 };
        const int16_t *pcm = (const int16_t *)(music_blob + 0x1000);
        uint16_t t0 = frt();
        for (int k = 0; k < 10; k++) mixv(acc, pcm, 0, args);
        music_bench = (uint16_t)(frt() - t0);
    }
#endif
    cycle_v = (SH2_HZ + MUS_RATE / 2) / MUS_RATE;
    blob = music_blob;
    echo_mem = echo;
    cur_seq = 0xFFFFFF;
    mp.playing = 0;
    mp.dsp.pwm_center = CYCLE / 2;
    for (int h = 0; h < 2; h++)
        for (int i = 0; i < HALF; i++) pwm_buf[h][i] = (CYCLE / 2) << 16 | (CYCLE / 2);

    REG16(PWM_CYC) = CYCLE + 1;
    REG16(PWM_CTRL) = 0x0185;               /* TM=1 (every sample), RTP (DREQ1), L->L, R->R */
    REG8(DRCR1) = 0;                        /* DREQ1 = external request */
    REG32(DMAOR) = 1;                       /* DME */
    REG32(VCRDMA1) = 63;                    /* vector 63 (slave VBR table entry, start.s) */
    REG16(IPRA) = (REG16(IPRA) & 0xF0FF) | (9 << 8);    /* below H (10) / V (12): the slave's sky interrupts */
    half = 0;
    dma_go(0);
    REG8(SYS_INTMASK) = REG8(SYS_INTMASK) | 0x01;   /* PWM int "enabled" (see above) */
    __asm__ volatile ("mov #0x60,r0\n\tldc r0,sr" ::: "r0");   /* I = 6 */
}
