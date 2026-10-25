/* native replacements for some DKC routines (tools/recomp/data/hle.txt) */
#define R65_GEN
#include "../tools/recomp/r65rt.h"

extern uint32_t apu_port0_echo(uint32_t v);

/* CODE_8AB4FF: upload $4F bytes from [$4C]+4 to SPC address (A.lo << 8 | A.hi) through the APU
   ports; the byte loop toggles $00 bit 7 per byte (the handshake).  Same WRAM / register results
   without the loop.  PHP .. PLP: flags and mode unchanged; the P byte is left on the stack page. */
static uint32_t apu_block(uint32_t mode)
{
    { int32_t l_n = r65_n; uint32_t l_v = r65_v; WL(s) = (uint8_t)PACKP(mode); }
    uint32_t dest = ((a & 0xFF) << 8) | ((a >> 8) & 0xFF);
    uint32_t t = WL(0x4C) + 4;
    WL(0x4C) = (uint8_t)t;
    if (t > 0xFF) WL(0x4D) = (uint8_t)(WL(0x4D) + 1);
    uint32_t size = WL(0x4F) | WL(0x50) << 8;
    uint32_t n = size ? size : 0x10000;
    uint32_t v = ((WL(0) ^ ((n & 1) << 7)) | 1) & 0xFF;
    WL(0) = (uint8_t)v;
    apu_port0_echo(v);
    a = ((a & 0xFF) << 8) | v;
    x = (dest + n) & 0xFFFF;
    y = n & 0xFFFF;
    uint32_t l, h;
    PULL8(l); PULL8(h);
    return (mode << 20) | 0x8A0000 | (((l | h << 8) + 1) & 0xFFFF);
}

uint32_t hle_apu_block(void) { return apu_block(0x00); }        /* M=0 X=0 */
uint32_t hle_apu_block_m2(void) { return apu_block(0x20); }     /* M=1 X=0 */

/* CODE_8AB4E6: [$4C] -> size (2), destination (2), then the block (CODE_8AB4FF) */
uint32_t game_apu_ptr, game_apu_n;      /* (the last block uploaded, count) */
uint32_t game_apu_song = 0xFF;            /* the music the game uploaded last (DATA_8AB159 index), 0xFF none */
uint32_t game_apu_song_n;                 /* (song uploads so far: the same song again restarts it) */
uint32_t hle_apu_upload(void)
{
    uint32_t ptr = WL(0x4C) | WL(0x4D) << 8 | WL(0x4E) << 16;
    game_apu_ptr = ptr; game_apu_n++;
    for (uint32_t i = 0; i < 0x1B; i++) {   /* (a song block: the music id; engine / effect blocks: not) */
        uint32_t t = 0x8AB159 + i * 3;
        if ((mrd8(t) | mrd8(t + 1) << 8 | mrd8(t + 2) << 16) == ptr) { game_apu_song = i; game_apu_song_n++; break; }
    }
    uint32_t b0 = mrd8(ptr), b1 = mrd8((ptr + 1) & 0xFFFFFF), b2 = mrd8((ptr + 2) & 0xFFFFFF), b3 = mrd8((ptr + 3) & 0xFFFFFF);
    WL(0x4F) = (uint8_t)b0; WL(0x50) = (uint8_t)b1;
    a = (b2 << 8) | b3;
    return apu_block(0x00);
}

/* CODE_80A203 (16-bit A/X): hide the unused OAM entries: $F0FF (x = $FF, y = $F0) every 4 bytes from the OAM
   index ($8E) to $0400.  Exit flags as after the final CPX #$0400 (Z=1 C=1 N=0); A = $F0FF if any
   entry was written; X = $0400. */
uint32_t hle_oam_clear(void)
{
    uint32_t i = WL(0x8E) | WL(0x8F) << 8;
    if (r65_d != 0 || i > 0x400 || (i & 3)) return interp(0x0080A203);     /* (mode 0 only: hle.txt) */
    if (i != 0x400) {
        for (; i < 0x400; i += 4) { WL(i) = 0xFF; WL(i + 1) = 0xF0; }
        a = 0xF0FF;
    }
    x = 0x400; z = 0; c = 1; r65_n = 0;
    uint32_t l, h;
    PULL8(l); PULL8(h);
    return 0x800000 | (((l | h << 8) + 1) & 0xFFFF);
}


/* ---------------------------------------------------------------------------------------------
   CODE_BBA849: the sprite OAM builder (~35% of the game's 65816 instructions per frame).  For each
   sprite in draw order: pose -> header + per-tile offsets (DATA_BBCC9C), cull against the camera,
   write OAM entries (groups: 16x16, 8x8 A, 8x8 B; the X / Y flip variants BBAAC4..BBAFAD) and
   queue the pose's graphics DMA ($170F table).  Same WRAM results as the 65816 code, direct page
   temporaries included (D must be 0); the DP state lives in locals and is written back at the
   end of each pose.  Exit as CODE_BBAA37: DB = $80, $170D = 0, RTL.  (SDRAM: hot.) */
extern R65_HOT void io_wr(uint32_t ea, uint32_t v);
/* the builder loop's code must stay in the 4 KB cache (MiSTer: SDRAM line fills ~30 cycles) */
#ifndef HLE_OPT
#define HLE_OPT __attribute__((optimize("Os")))
#endif
extern uint8_t wram_lo[0x2000];
#define W16(a)     ((uint32_t)(W[a] | W[(a) + 1] << 8))
#define W16S(a, v) do { uint32_t _v = (v); W[a] = (uint8_t)_v; W[(a) + 1] = (uint8_t)(_v >> 8); } while (0)

static const uint8_t oam_bits[132] = {
    0x03, 0x0C, 0x30, 0xC0,
    1, 4, 16, 64, 1, 4, 16, 64, 1, 4, 16, 64, 1, 4, 16, 64, 1, 4, 16, 64, 1, 4, 16, 64, 1, 4, 16, 64, 1, 4, 16, 64,
    1, 4, 16, 64, 1, 4, 16, 64, 1, 4, 16, 64, 1, 4, 16, 64, 1, 4, 16, 64, 1, 4, 16, 64, 1, 4, 16, 64, 1, 4, 16, 64,
    1, 4, 16, 64, 1, 4, 16, 64, 1, 4, 16, 64, 1, 4, 16, 64, 1, 4, 16, 64, 1, 4, 16, 64, 1, 4, 16, 64, 1, 4, 16, 64,
    1, 4, 16, 64, 1, 4, 16, 64, 1, 4, 16, 64, 1, 4, 16, 64, 1, 4, 16, 64, 1, 4, 16, 64, 1, 4, 16, 64, 1, 4, 16, 64 };

static uint8_t pose_buf[784];          /* the pose header + tile offsets ([$5A] + 0 ..) */

/* bytes ea .. ea+n-1 of 65816 memory (24-bit linear, like [dp],y) */
static SDRAM_FN HLE_OPT void pose_copy(uint8_t *d, uint32_t ea, uint32_t n)
{
    while (n) {
        uint32_t bank = (ea >> 16) & 0xFF;
        if (!(bank & 0x40) || (bank & 0xFE) == 0x7E) { *d++ = (uint8_t)mrd8(ea); ea = (ea + 1) & 0xFFFFFF; n--; continue; }
        uint32_t o = ea & 0x3FFFFF, k = 0x400 - (o & 0x3FF);
        if (k > n) k = n;
        const uint8_t *s = &ROMB(o);
        for (uint32_t i = 0; i < k; i++) d[i] = s[i];
        d += k; ea = (ea + k) & 0xFFFFFF; n -= k;
    }
}

/* (I13) the pose bytes ea .. ea+n-1 in place when they sit inside one 1 KB page of the ROM blob (most
   poses: no copy), else copied into pose_buf */
static SDRAM_FN HLE_OPT const uint8_t *pose_src(uint32_t ea, uint32_t n)
{
    uint32_t bank = (ea >> 16) & 0xFF, o = ea & 0x3FFFFF;
    if ((bank & 0x40) && (bank & 0xFE) != 0x7E && (o & 0x3FF) + n <= 0x400) return &ROMB(o);
    pose_copy(pose_buf, ea, n);
    return pose_buf;
}

/* DEC until negative: iterations, final byte */
#define DEC_IT(n) (((n) >= 1 && (n) <= 0x80) ? (n) : 0)
#define DEC_FIN(n) ((n) >= 0x81 ? (uint8_t)((n) - 1) : (uint8_t)0xFF)

/* one pose (the six JSR'd variants): 0, or 1 when the OAM would overflow (the 65816 code then
   PLAs its return address and leaves the builder); *py = Y (offset into the pose data) */
static SDRAM_FN HLE_OPT int oam_pose(uint8_t *W, int xf, int yf, int hdr8, uint32_t *py)
{
    uint32_t base = W16(0x5A) | (uint32_t)W[0x5C] << 16;
    const uint8_t *h = pose_src(base, 8);
    W[0x50] = h[0]; W[0x51] = h[1]; W[0x52] = h[2]; W[0x53] = h[3]; W[0x54] = h[4]; W[0x55] = h[5];
    uint32_t yy = 5;
    if (hdr8) { W[0x56] = h[6]; W[0x57] = h[7]; yy = 8; }
    uint32_t n16 = h[0], na = h[1], nb = h[3], ba = h[2], bb = h[4];
    uint32_t oi = W16(0x8E);
    if (!((((oi >> 2) & 0xFF) + n16) & 0x80)) { *py = yy; return 1; }
    uint32_t it16 = DEC_IT(n16), ita = DEC_IT(na), itb = DEC_IT(nb);
    uint32_t need = yy + 2 * (it16 + ita + itb) + 1;
    if (need > sizeof pose_buf) need = sizeof pose_buf;       /* (never: <= 776) */
    const uint8_t *d;
    if ((base & 0xFFFF) == 0xFFFF) {
        /* [$5E] = bank:0000 (the 16-bit INC wrapped): the y bytes come from there */
        pose_copy(pose_buf, base, need);
        for (uint32_t i = yy + 1; i < need; i += 2) pose_buf[i] = (uint8_t)mrd8(((base & 0xFF0000) + i - 1) & 0xFFFFFF);
        d = pose_buf;
    } else d = pose_src(base, need);
    uint32_t hp = (((oi >> 2) & 0xFF) >> 2 & 0x1F) | (uint32_t)W[0x6F] << 8;
    uint32_t m58 = oam_bits[(oi >> 2) & 3];
    uint32_t t4e = W16(0x4E), v90 = W16(0x90), v92 = W16(0x92), xx = oi, cy = 0, v6c = 0x10000;
    const uint8_t *q = d + yy;
    /* group 1: 16 x 16 */
    for (uint32_t k = 0; k < it16; k++, q += 2) {
        uint32_t v = q[1]; if (yf) v ^= 0xFF;
        uint32_t A = v + v92 + cy;
        cy = A >> 16; A &= 0xFFFF;
        if (A < 0xF0) {
            W[xx + 1] = (uint8_t)(A - 0x10);
            uint32_t b = q[0]; if (xf) b ^= 0xFF;
            uint32_t X = (b + v90) & 0xFFFF;
            W[xx] = (uint8_t)X;
            W[hp] |= (uint8_t)(X < 0x100 ? m58 & 0xAA : m58);
            if (m58 & 0x80) { hp = (hp & 0xFF00) | ((hp + 1) & 0xFF); m58 = 3; } else m58 = (m58 << 2) & 0xFF;
            W[xx + 2] = (uint8_t)t4e; W[xx + 3] = (uint8_t)(t4e >> 8);
            xx = (xx + 4) & 0xFFFF;
        }
        cy = 0;
        t4e = (t4e + 2) & 0xFFFF;
        if (t4e & 0x10) { t4e += 0x10; cy = t4e >> 16; t4e &= 0xFFFF; }
    }
    W[0x50] = DEC_FIN(n16);
    yy += 2 * it16;
    int ab = 0;
    /* groups 2 and 3: 8 x 8 */
    for (int g = 0; g < 2 && !ab; g++) {
        uint32_t n = g ? nb : na, it = g ? itb : ita;
        if (!((((xx >> 2) & 0xFF) + n) & 0x80)) { ab = 1; break; }
        t4e = (t4e & 0xFF00) | ((((g ? bb : ba)) + W[0x4C]) & 0xFF);
        if (!g) {
            if (xf) v90 = (v90 + 8) & 0xFFFF;
            if (yf) v92 = (v92 + 8) & 0xFFFF;
        }
        for (uint32_t k = 0; k < it; k++, q += 2) {
            uint32_t v = q[1]; if (yf) v ^= 0xFF;
            uint32_t A = (v + v92) & 0xFFFF;
            if (A < 0xF0) {
                W[xx + 1] = (uint8_t)(A - 0x10);
                uint32_t b = q[0]; if (xf) b ^= 0xFF;
                uint32_t X = (b + v90) & 0xFFFF;
                W[xx] = (uint8_t)X;
                if (X & 0x100) {
                    v6c = xx;
                    uint32_t e = (xx & 0x1FC) >> 2;
                    hp = (hp & 0xFF00) | (e >> 2);
                    W[hp] |= oam_bits[4 + e];
                }
                W[xx + 2] = (uint8_t)t4e; W[xx + 3] = (uint8_t)(t4e >> 8);
                xx = (xx + 4) & 0xFFFF;
            }
            t4e = (t4e + 1) & 0xFFFF;
        }
        W[g ? 0x53 : 0x51] = DEC_FIN(n);
        yy += 2 * it;
    }
    W16S(0x4E, t4e); W[0x58] = (uint8_t)m58; W[0x6E] = (uint8_t)hp;
    W16S(0x90, v90); W16S(0x92, v92);
    if (v6c < 0x10000) W16S(0x6C, v6c);
    W16S(0x8E, xx);
    *py = yy;
    return ab;
}

SDRAM_FN HLE_OPT uint32_t hle_oam_build(void)
{
    if (r65_d != 0) return interp(0x00BBA849);
    uint8_t *const W = wram_lo;
    uint32_t A = 0, l, h, b;
    W16S(0x08AB, 0);
    for (;;) {
        /* BBA84C */
        if (W16(0x8E) == 0x0400) {                         /* OAM full: INIDISP = 7, leave */
            io_wr(0x002100, 7);
            a = 0x0407;                                     /* (B = $04 from LDA $8E) */
            W[(s - 1) & 0x1FFF] = 0x80; W[s & 0x1FFF] = 0x80;   /* PEA $8080 / PLB / PLB */
            r65_db = 0x80; r65_n = -128; z = 1; c = 1;
            PULL8(l); PULL8(h); PULL8(b);
            return b << 16 | (((l | h << 8) + 1) & 0xFFFF);
        }
        uint32_t k = W16(0x08AB);
        uint32_t yv = W16(0x0AB1 + k);
        if (W16(0x0D45 + yv) == 0) goto next;
        uint32_t xv = W16(0x0D11 + yv);
        if (W16(0x170D) >= W16(0x3C) && xv >= 0x8D) xv = W16(0x0AE5 + yv);
        W16S(0x0CDD + yv, xv);
        if (!xv) goto next;
        {
            const uint8_t *pt = &ROMB((0x3BCC9C + xv) & 0x3FFFFF);
            uint32_t p, q;
            if (((0x3BCC9C + xv) & 0x3FF) <= 0x3FC) { p = pt[0] | pt[1] << 8; q = pt[2] | pt[3] << 8; }
            else { p = mrd8(0xBBCC9C + xv) | mrd8(0xBBCC9D + xv) << 8; q = mrd8(0xBBCC9E + xv) | mrd8(0xBBCC9F + xv) << 8; }
            W16S(0x5A, p); W16S(0x5E, p + 1);
            W16S(0x5C, q); W16S(0x60, q);
            W16S(0x0A7D + yv, (q >> 8) + W16(0x0B8D + yv));
        }
        {
            uint32_t cp = W16(0x0D11 + yv);
            int big = ((cp - 0x2670) & 0xFFFF) < 0x5C;
            uint32_t t = (W16(0x4A) - W16(0x0BC1 + yv) - 0x80 - W16(0x0895)) & 0xFFFF;
            uint32_t sm = t + W16(0x0887);
            W16S(0x92, sm);
            uint32_t chk = (sm & 0xFFFF) + 0x90 + (sm >> 16);
            if ((chk & 0xFFFF) >= (big ? 0x150 : 0x130)) goto next;
            uint32_t xp = (W16(0x0B19 + yv) - W16(0x088B) - 1) & 0xFFFF;   /* SBC with C = 0 (the CMP) */
            W16S(0x90, xp);
            /* x cull limits (the widescreen patch widens them: read from the ROM image) */
            const uint8_t *cl = &ROMB(big ? 0x3BA911 : 0x3BA8E1), *ca = &ROMB(big ? 0x3BA90E : 0x3BA8DE);
            if (((xp + (uint32_t)(ca[0] | ca[1] << 8)) & 0xFFFF) >= (uint32_t)(cl[0] | cl[1] << 8)) goto next;
        }
        {
            uint32_t attr = W16(0x0C69 + yv), yy = 0;
            W16S(0x4C, attr); W16S(0x4E, attr);
            int dma, xf = (attr & 0x4000) != 0;
            if (attr & 0x8000) {
                W16S(0x90, W16(0x90) - (xf ? 0x8F : 0x80));
                W16S(0x92, W16(0x92) + 1);
                dma = 1;
                if (oam_pose(W, xf, 1, 1, &yy)) { A = xf ? 0xA989 : 0xA97A; break; }
            } else {
                W16S(0x90, W16(0x90) - (xf ? 0x8F : 0x80));
                W16S(0x92, W16(0x92) + 0x10);
                dma = xv >= 0x8C;
                if (oam_pose(W, xf, 0, dma, &yy)) { A = xf ? (dma ? 0xA966 : 0xA960) : (dma ? 0xA946 : 0xA940); break; }
            }
            if (!dma) goto next;
            /* BBA98A: queue the pose's graphics DMA */
            if (W16(0x170D) >= W16(0x3C)) goto next;
            uint32_t sx = W16(0x0AB1 + W16(0x08AB));
            uint32_t cp = W16(0x0D11 + sx);
            if (cp == W16(0x0AE5 + sx)) goto next;
            W16S(0x0AE5 + sx, cp);
            uint32_t X = W16(0x170D);
            uint32_t Y = (yy + W16(0x5A)) & 0xFFFF;
            W16S(0x1713 + X, Y);
            uint32_t sz = (W[0x55] << 5) & 0xFFFF;
            W16S(0x170F + X, sz); W16S(0x6C, sz);
            Y = (Y + sz) & 0xFFFF;
            uint32_t va = ((W16(0x4C) & 0x1FF) << 4) & 0xFFFF;
            W16S(0x1711 + X, va);
            W16S(0x1715 + X, W16(0x5C) | 0xFF00);
            X = (X + 8) & 0xFFFF;
            W16S(0x1715 + X, 0);
            if (W[0x57] & 0x0F) {
                W16S(0x170F + X, (W[0x57] & 0x0F) << 5);
                uint32_t t6 = (W[0x56] << 4) & 0xFFFF;
                W16S(0x6C, t6);
                W16S(0x1711 + X, (va + t6) & 0xFFFF);
                W16S(0x1713 + X, Y);
                W16S(0x1715 + X, W16(0x5C) | 0xFF00);
                X = (X + 8) & 0xFFFF;
            }
            W16S(0x170D, X);
            W16S(0x1715 + X, 0);
        }
    next:
        W16S(0x08AB, W16(0x08AB) + 2);
        if (W16(0x08AB) == 0x34) { A = 0x34; break; }
    }
    /* BBAA37: PEA $8080; PLB; PLB; STZ $170D; RTL (the PEA bytes stay below S) */
    W[(s - 1) & 0x1FFF] = 0x80; W[s & 0x1FFF] = 0x80;
    W16S(0x170D, 0);
    a = A; r65_db = 0x80; r65_n = -128; z = 1; c = 1;
    PULL8(l); PULL8(h); PULL8(b);
    return b << 16 | (((l | h << 8) + 1) & 0xFFFF);
}

/* CODE_BBA845: JSR CODE_BBAADF (no flip, 8-byte header); RTL - the single-sprite path (BBA772).
   An OAM overflow leaves through CODE_BBAA37 (PLA, PEA $8080 / PLB / PLB, STZ $170D, RTL). */
SDRAM_FN uint32_t hle_oam_pose845(void)
{
    if (r65_d != 0) return interp(0x00BBA845);
    uint8_t *const W = wram_lo;
    uint32_t yy, l, h, b;
    int ab = oam_pose(W, 0, 0, 1, &yy);
    y = yy;
    if (ab) {
        W[(s - 1) & 0x1FFF] = 0x80; W[s & 0x1FFF] = 0x80;
        W16S(0x170D, 0);
        a = 0xA847; r65_db = 0x80; r65_n = -128; z = 1;
    } else {
        W[(s - 1) & 0x1FFF] = 0x47; W[s & 0x1FFF] = 0xA8;     /* the JSR's return address */
        x = W16(0x8E); r65_n = -128; z = 1;
    }
    PULL8(l); PULL8(h); PULL8(b);
    return b << 16 | (((l | h << 8) + 1) & 0xFFFF);
}

/* CODE_80C246 (16-bit A / X): the camera target X ($1A5E) clamped to the level's minimum ($1B23),
   the target Y ($1A50) to >= 0, then CODE_B8A853 (camera blocks: clamps the target to the current
   block's range [$50, $52] x [$54, $56]).  Same results; the X range is kept for the renderer
   (main.c: the 32X view, 64 px wider, stops at the range's right end instead of showing what lies
   past it). */
volatile uint32_t hle_cam_xmin, hle_cam_xmax, hle_cam_frame;
uint32_t hle_cam_clamp(void)
{
    int32_t l_n; uint32_t l_v = r65_v;
    uint32_t t = WL(0x1A5E) | WL(0x1A5F) << 8, b = WL(0x1B23) | WL(0x1B24) << 8;
    c = b >= t;                                          /* CMP $1A5E */
    if (!c) {                                            /* BIT $1A5E (A = $1B23) */
        l_v = (t >> 14) & 1;
        if (t & 0x8000) { WL(0x1A5E) = (uint8_t)b; WL(0x1A5F) = (uint8_t)(b >> 8); }
    } else { WL(0x1A5E) = (uint8_t)b; WL(0x1A5F) = (uint8_t)(b >> 8); }
    a = WL(0x1A50) | WL(0x1A51) << 8;                    /* LDA $1A50: BPL / STZ */
    if (a & 0x8000) { WL(0x1A50) = 0; WL(0x1A51) = 0; }
    z = a; l_n = (int16_t)a;
    r65_n = l_n; r65_v = l_v;
    uint32_t r = r65_callx3(0x00B8A853, 0x0080C262);     /* JSL CODE_B8A853 */
    if (r) return r;
    {
        hle_cam_xmin = WL(0x50) | WL(0x51) << 8;
        hle_cam_xmax = WL(0x52) | WL(0x53) << 8;
        hle_cam_frame++;
    }
    uint32_t l, h;
    PULL8(l); PULL8(h);
    return 0x800000 | (((l | h << 8) + 1) & 0xFFFF);
}

/* CODE_80A7ED (16-bit A / X, D = 0): a vertical rope (sprite X of the $1B2B list, CODE_80A7BE: $50 = 3,
   $52 = $28) as a column of 16 x 16 tiles (tile $060, the sprite's attributes without X flip) upwards
   from its bottom end, as many as fit ($4A - y .. 0) and the OAM has room for.  (I11) The 65816 code
   drew nothing of a rope outside SNES x 0..255: the ropes vanished in the 32X's extra columns
   (Forest Frenzy).  Same results inside that range; outside it (x -80..319) the tiles are written
   with OAM x bit 8 (sprites.c: 256..319 on screen, 448.. = negative). */
/* (I14) byte-wise: the sprite tables ($0B19 + X ...) and $088B / $0895 sit at ODD addresses - WLW is a 16-bit
   load (constant even addresses only): the SH-2 reads the aligned word instead (PicoDrive / MiSTer) or
   takes an address error.  The I11 version read garbage positions: the ropes' upper columns were never
   drawn.  (The host sim reads unaligned words fine - it passed the byte-exact checks.) */
#define W16L(a) ((uint32_t)(WL(a) | WL((a) + 1) << 8))
#define W16L_SET(a, v) do { uint32_t _a = (a), _v = (v); WL(_a) = (uint8_t)_v; WL(_a + 1) = (uint8_t)(_v >> 8); } while (0)
#ifdef ROPEDBG
volatile uint32_t rope_dbg[8];
#endif
uint32_t hle_rope_col(void)
{
#ifdef R65_HOST_SIM
    { extern char *getenv(const char *); extern int printf(const char *, ...);
      if (getenv("ROPELOG")) printf("rope call X %X d %X x %d y %d 4A %d 50 %d 52 %d\n", x, r65_d, W16L(0xB19 + (x & 0xFFFF)) - W16L(0x88B), W16L(0xBC1 + (x & 0xFFFF)), W16L(0x4A), W16L(0x50), W16L(0x52)); }
#endif
    if (r65_d != 0) return interp(0x0080A7ED);
#ifdef ROPEDBG
    { extern volatile uint32_t rope_dbg[8]; rope_dbg[0]++; rope_dbg[1] = x; }
#endif
    uint32_t X = x & 0xFFFF;
    uint32_t t4c = (W16L(0xC69 + X) & 0xBE00) | 0x0060;
    W16L_SET(0x4C, t4c);
    int32_t rx = (int16_t)(W16L(0xB19 + X) - W16L(0x88B) - W16L(0x50));
    W16L_SET(0x76, (uint32_t)rx & 0xFFFF);
    a = (uint32_t)rx & 0xFFFF;
    if (rx >= -80 && rx < 320) {
        int32_t ry = (int16_t)(W16L(0x4A) - W16L(0xBC1 + X) - 1 - W16L(0x895) - W16L(0x52));
        uint32_t n = 0, v76;
        if (ry >= 0) {
            if (ry >= 0xE0) ry = (ry & 0xF) | 0xE0;
            W16L_SET(0x78, (uint32_t)ry);
            v76 = ((uint32_t)ry & 0xFF) << 8 | ((uint32_t)rx & 0xFF);
            n = ((uint32_t)(ry + 15) >> 4) & 0xF;
            if (n) n++;
            else W16L_SET(0x76, v76);
        } else if (ry >= -15) {
            W16L_SET(0x78, (uint32_t)ry & 0xFFFF);
            v76 = ((uint32_t)ry & 0xFF) << 8 | ((uint32_t)rx & 0xFF);
            n = 2;
        } else
            n = 0, v76 = 0;
        if (n) {
            W16L_SET(0x76, v76);
            uint32_t Y = W16L(0x8E);
            int32_t room = (int16_t)(0x400 - Y);
            if (room >= 0) {
                uint32_t rem = (uint32_t)room >> 2;
#ifdef ROPEDBG
                { extern volatile uint32_t rope_dbg[8]; rope_dbg[2] = (uint32_t)rx; rope_dbg[3] = (uint32_t)ry; rope_dbg[4] = n; rope_dbg[5] = rem; rope_dbg[6]++; }
#endif
#ifdef R65_HOST_SIM
                { extern char *getenv(const char *); extern int printf(const char *, ...);
                  if (getenv("ROPELOG")) printf("rope x %d ry %d n %u rem %u oamidx %03X\n", rx, ry, n, rem, Y); }
#endif
                W16L_SET(0x56, rem);
                if (n > rem) n = rem;
                uint32_t hb = rx < 0 || rx >= 256;          /* OAM x bit 8 */
                for (; n; n--) {
                    W16L_SET(Y, v76);
                    W16L_SET(Y + 2, t4c);
                    uint32_t slot = (Y - 0x200) >> 2;
                    WL(0x400 + (slot >> 2)) |= (uint8_t)((2 | hb) << (2 * (slot & 3)));
                    Y += 4;
                    v76 = (v76 - 0x1000) & 0xFFFF;
                }
                W16L_SET(0x8E, Y);
                a = v76;
            }
        }
    }
    uint32_t l, h;
    PULL8(l); PULL8(h);
    return 0x800000 | (((l | h << 8) + 1) & 0xFFFF);
}

/* (I13) CODE_B88344 (16-bit A / X, JSL): the sprite draw order ($0AB1.. : 26 slot offsets) - one
   bubble pass up (swap when the next key is greater), one down (swap when smaller); key = the word at
   $0A7D + slot (data bank DB).  $6C = the last key compared.  Same WRAM and registers: A as the 65816
   code leaves it, X = $0AAF, Y = the last slot read, C = 1 / Z = 1 from the final CPX (N, V untouched).
   ~2.5 K SH-2 instructions per frame as recompiled code. */
SDRAM_FN uint32_t hle_spr_sort(void)
{
    if (r65_d != 0) return interp(0x00B88344);
    const uint32_t db = r65_db;
    uint32_t A, Y, X, m;
    Y = WL(0x0AB1) | WL(0x0AB2) << 8;
    m = rd16_sys(db, 0x0A7D + Y);
    for (X = 0x0AAF; X != 0x0AE1; X += 2) {
        Y = rd16_b0(X + 4);
        A = rd16_sys(db, 0x0A7D + Y);
        int up = A > m;
        m = A;
        if (up) { A = rd16_b0(X + 2); wr16_b0(X + 4, A); wr16_b0(X + 2, Y); }
    }
    do {
        Y = rd16_b0(X);
        A = rd16_sys(db, 0x0A7D + Y);
        int lt = A < m;
        m = A;
        if (lt) { A = rd16_b0(X + 2); wr16_b0(X, A); wr16_b0(X + 2, Y); }
        X -= 2;
    } while (X != 0x0AAF);
    WL(0x6C) = (uint8_t)m; WL(0x6D) = (uint8_t)(m >> 8);
    a = A; x = X; y = Y; c = 1; z = 0;
    uint32_t l, h, b;
    PULL8(l); PULL8(h); PULL8(b);
    return b << 16 | (((l | h << 8) + 1) & 0xFFFF);
}
