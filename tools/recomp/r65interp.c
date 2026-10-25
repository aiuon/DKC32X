/* r65interp: 65816 interpreter for code that has no translated function (cold code, mid-function
   resume points after stack tricks).  Same protocol as a translated function: runs from `key`
   (mode << 24 | pc) and returns the key an RTS / RTL / RTI produced, WAITKEY | resume at WAI, or
   ERRKEY | pc.  Calls go through xcall() (translated or interpreted). Native mode only, D flag
   assumed clear (DKC never sets it). */
#define R65_REGS
#include "r65rt.h"

#define A r65_a
#define X r65_x
#define Y r65_y
#define S r65_s
#define D r65_d
#define DB r65_db
#define MX r65_mx

static inline uint32_t fetch8(uint32_t pc) { return ROMB(((pc >> 16) & 0x3F) << 16 | (pc & 0xFFFF)); }

#define PUSHB(v) do { WL(S) = (uint8_t)(v); S = (S - 1) & 0xFFFF; } while (0)
#define PULLB(dst) do { S = (S + 1) & 0xFFFF; (dst) = WL(S); } while (0)

static inline void setnz8(uint32_t r) { r65_z = r & 0xFF; r65_n = (int8_t)r; }
static inline void setnz16(uint32_t r) { r65_z = r & 0xFFFF; r65_n = (int16_t)r; }

uint32_t r65_ibudget;
volatile uint32_t r65_dbg_pc;
uint32_t interp(uint32_t key)
{
    uint32_t pc = key & 0xFFFFFF;
    MX = (key >> 20) & 0x30;
    for (;;) {
#ifdef R65_HOST
        { extern long r65_icount; extern void r65_watchdog(uint32_t); extern uint32_t *r65_iprof; r65_icount++; if (!(r65_icount & 0xFFFFF)) r65_watchdog(pc);
          if (r65_iprof) r65_iprof[pc & 0xFFFFFF]++; }
#else
        if (++r65_ibudget > 4000000) return ERRKEY | 0x0D000000 | pc;     /* stuck: report it */
#endif
        { extern volatile uint32_t r65_dbg_pc; r65_dbg_pc = pc; }
#ifdef R65_ITRACE
        { extern void r65_itrace(uint32_t); r65_itrace(pc); }
#endif
        uint32_t op = fetch8(pc);
        uint32_t pb = pc & 0xFF0000;
#define PCN(n) (pb | ((pc + (n)) & 0xFFFF))
        uint32_t b1 = fetch8(PCN(1)), b2 = fetch8(PCN(2)), b3 = fetch8(PCN(3));
        uint32_t w = b1 | b2 << 8;
        int m8 = (MX & 0x20) != 0, x8 = (MX & 0x10) != 0;
        uint32_t ea = 0, len = 1, v, t;
        int mem = 0;     /* 1 = ea is 24-bit, 2 = ea is bank 0 (wrap 16) */
        /* addressing mode decode for the regular groups */
        switch (op) {
        /* dp */
        case 0x04: case 0x05: case 0x06: case 0x14: case 0x24: case 0x25: case 0x26: case 0x45: case 0x46:
        case 0x64: case 0x65: case 0x66: case 0x84: case 0x85: case 0x86: case 0xA4: case 0xA5: case 0xA6:
        case 0xC4: case 0xC5: case 0xC6: case 0xE4: case 0xE5: case 0xE6:
            ea = (D + b1) & 0xFFFF; mem = 2; len = 2; break;
        /* dp,x */
        case 0x15: case 0x16: case 0x34: case 0x35: case 0x36: case 0x55: case 0x56: case 0x74: case 0x75:
        case 0x76: case 0x94: case 0x95: case 0xB4: case 0xB5: case 0xD5: case 0xD6: case 0xF5: case 0xF6:
            ea = (D + b1 + X) & 0xFFFF; mem = 2; len = 2; break;
        /* dp,y */
        case 0x96: case 0xB6:
            ea = (D + b1 + Y) & 0xFFFF; mem = 2; len = 2; break;
        /* (dp) */
        case 0x12: case 0x32: case 0x52: case 0x72: case 0x92: case 0xB2: case 0xD2: case 0xF2:
            t = (D + b1) & 0xFFFF; ea = DB << 16 | rd16_b0(t); mem = 1; len = 2; break;
        /* (dp,x) */
        case 0x01: case 0x21: case 0x41: case 0x61: case 0x81: case 0xA1: case 0xC1: case 0xE1:
            t = (D + b1 + X) & 0xFFFF; ea = DB << 16 | rd16_b0(t); mem = 1; len = 2; break;
        /* (dp),y */
        case 0x11: case 0x31: case 0x51: case 0x71: case 0x91: case 0xB1: case 0xD1: case 0xF1:
            t = (D + b1) & 0xFFFF; ea = ((DB << 16 | rd16_b0(t)) + Y) & 0xFFFFFF; mem = 1; len = 2; break;
        /* [dp] */
        case 0x07: case 0x27: case 0x47: case 0x67: case 0x87: case 0xA7: case 0xC7: case 0xE7:
            t = (D + b1) & 0xFFFF; ea = rd16_b0(t) | rd8_b0((t + 2) & 0xFFFF) << 16; mem = 1; len = 2; break;
        /* [dp],y */
        case 0x17: case 0x37: case 0x57: case 0x77: case 0x97: case 0xB7: case 0xD7: case 0xF7:
            t = (D + b1) & 0xFFFF; ea = ((rd16_b0(t) | rd8_b0((t + 2) & 0xFFFF) << 16) + Y) & 0xFFFFFF; mem = 1; len = 2; break;
        /* abs */
        case 0x0C: case 0x0D: case 0x0E: case 0x1C: case 0x2C: case 0x2D: case 0x2E: case 0x4D: case 0x4E:
        case 0x6D: case 0x6E: case 0x8C: case 0x8D: case 0x8E: case 0x9C: case 0xAC: case 0xAD: case 0xAE:
        case 0xCC: case 0xCD: case 0xCE: case 0xEC: case 0xED: case 0xEE:
            ea = DB << 16 | w; mem = 1; len = 3; break;
        /* abs,x */
        case 0x1D: case 0x1E: case 0x3C: case 0x3D: case 0x3E: case 0x5D: case 0x5E: case 0x7D: case 0x7E:
        case 0x9D: case 0x9E: case 0xBC: case 0xBD: case 0xDD: case 0xDE: case 0xFD: case 0xFE:
            ea = ((DB << 16 | w) + X) & 0xFFFFFF; mem = 1; len = 3; break;
        /* abs,y */
        case 0x19: case 0x39: case 0x59: case 0x79: case 0x99: case 0xB9: case 0xBE: case 0xD9: case 0xF9:
            ea = ((DB << 16 | w) + Y) & 0xFFFFFF; mem = 1; len = 3; break;
        /* long */
        case 0x0F: case 0x2F: case 0x4F: case 0x6F: case 0x8F: case 0xAF: case 0xCF: case 0xEF:
            ea = w | b3 << 16; mem = 1; len = 4; break;
        /* long,x */
        case 0x1F: case 0x3F: case 0x5F: case 0x7F: case 0x9F: case 0xBF: case 0xDF: case 0xFF:
            ea = ((w | b3 << 16) + X) & 0xFFFFFF; mem = 1; len = 4; break;
        /* sr */
        case 0x03: case 0x23: case 0x43: case 0x63: case 0x83: case 0xA3: case 0xC3: case 0xE3:
            ea = (S + b1) & 0xFFFF; mem = 2; len = 2; break;
        /* (sr),y */
        case 0x13: case 0x33: case 0x53: case 0x73: case 0x93: case 0xB3: case 0xD3: case 0xF3:
            t = (S + b1) & 0xFFFF; ea = ((DB << 16 | rd16_b0(t)) + Y) & 0xFFFFFF; mem = 1; len = 2; break;
        }
#define RD(wid) ((wid) == 8 ? (mem == 2 ? rd8_b0(ea) : mrd8(ea)) : (mem == 2 ? rd16_b0(ea) : mrd16(ea)))
#define WR(wid, val) do { if ((wid) == 8) { if (mem == 2) wr8_b0(ea, (val)); else mwr8(ea, (val)); } \
                          else { if (mem == 2) wr16_b0(ea, (val)); else mwr16(ea, (val)); } } while (0)
        uint32_t wa = m8 ? 8 : 16, wx = x8 ? 8 : 16, mka = m8 ? 0xFF : 0xFFFF, mkx = x8 ? 0xFF : 0xFFFF;
        uint32_t lo = op & 0x1F, grp = op >> 5;
        /* immediate operands */
        int immA = (op & 0x1F) == 0x09 && op != 0x89 ? 1 : 0;
        if (op == 0x89) immA = 1;
        if (immA) { v = m8 ? b1 : w; len = m8 ? 2 : 3; }
        (void)lo; (void)grp;
        switch (op) {
        /* ---- ORA / AND / EOR / ADC / CMP / SBC / LDA / STA */
        case 0x01: case 0x03: case 0x05: case 0x07: case 0x09: case 0x0D: case 0x0F: case 0x11: case 0x12:
        case 0x13: case 0x15: case 0x17: case 0x19: case 0x1D: case 0x1F:
            if (!immA) v = RD(wa);
            t = (A | v) & mka; if (m8) { A = (A & 0xFF00) | t; setnz8(t); } else { A = t; setnz16(t); } break;
        case 0x21: case 0x23: case 0x25: case 0x27: case 0x29: case 0x2D: case 0x2F: case 0x31: case 0x32:
        case 0x33: case 0x35: case 0x37: case 0x39: case 0x3D: case 0x3F:
            if (!immA) v = RD(wa);
            t = (A & v) & mka; if (m8) { A = (A & 0xFF00) | t; setnz8(t); } else { A = t; setnz16(t); } break;
        case 0x41: case 0x43: case 0x45: case 0x47: case 0x49: case 0x4D: case 0x4F: case 0x51: case 0x52:
        case 0x53: case 0x55: case 0x57: case 0x59: case 0x5D: case 0x5F:
            if (!immA) v = RD(wa);
            t = (A ^ v) & mka; if (m8) { A = (A & 0xFF00) | t; setnz8(t); } else { A = t; setnz16(t); } break;
        case 0x61: case 0x63: case 0x65: case 0x67: case 0x69: case 0x6D: case 0x6F: case 0x71: case 0x72:
        case 0x73: case 0x75: case 0x77: case 0x79: case 0x7D: case 0x7F:
        case 0xE1: case 0xE3: case 0xE5: case 0xE7: case 0xE9: case 0xED: case 0xEF: case 0xF1: case 0xF2:
        case 0xF3: case 0xF5: case 0xF7: case 0xF9: case 0xFD: case 0xFF:
            if (!immA) v = RD(wa);
            if (op >= 0xE0) v ^= mka;
            t = (A & mka) + v + r65_c;
            r65_v = ((~(A ^ v) & (v ^ t)) >> (wa - 1)) & 1;
            r65_c = t >> wa;
            t &= mka;
            if (m8) { A = (A & 0xFF00) | t; setnz8(t); } else { A = t; setnz16(t); }
            break;
        case 0xC1: case 0xC3: case 0xC5: case 0xC7: case 0xC9: case 0xCD: case 0xCF: case 0xD1: case 0xD2:
        case 0xD3: case 0xD5: case 0xD7: case 0xD9: case 0xDD: case 0xDF:
            if (!immA) v = RD(wa);
            t = A & mka; r65_c = t >= v; if (m8) setnz8(t - v); else setnz16(t - v); break;
        case 0xA1: case 0xA3: case 0xA5: case 0xA7: case 0xA9: case 0xAD: case 0xAF: case 0xB1: case 0xB2:
        case 0xB3: case 0xB5: case 0xB7: case 0xB9: case 0xBD: case 0xBF:
            if (!immA) v = RD(wa);
            if (m8) { A = (A & 0xFF00) | v; setnz8(v); } else { A = v; setnz16(v); } break;
        case 0x81: case 0x83: case 0x85: case 0x87: case 0x8D: case 0x8F: case 0x91: case 0x92: case 0x93:
        case 0x95: case 0x97: case 0x99: case 0x9D: case 0x9F:
            WR(wa, A); break;
        case 0x64: case 0x74: case 0x9C: case 0x9E:
            WR(wa, 0); break;
        /* ---- index loads / stores / compares */
        case 0xA0: case 0xA2: case 0xC0: case 0xE0:
            v = x8 ? b1 : w; len = x8 ? 2 : 3;
            if (op == 0xA0) { Y = v; x8 ? setnz8(v) : setnz16(v); }
            else if (op == 0xA2) { X = v; x8 ? setnz8(v) : setnz16(v); }
            else { t = (op == 0xC0 ? Y : X) & mkx; r65_c = t >= v; x8 ? setnz8(t - v) : setnz16(t - v); }
            break;
        case 0xA4: case 0xB4: case 0xAC: case 0xBC: v = RD(wx); Y = v; x8 ? setnz8(v) : setnz16(v); break;
        case 0xA6: case 0xB6: case 0xAE: case 0xBE: v = RD(wx); X = v; x8 ? setnz8(v) : setnz16(v); break;
        case 0x84: case 0x94: case 0x8C: WR(wx, Y); break;
        case 0x86: case 0x96: case 0x8E: WR(wx, X); break;
        case 0xC4: case 0xCC: v = RD(wx); t = Y & mkx; r65_c = t >= v; x8 ? setnz8(t - v) : setnz16(t - v); break;
        case 0xE4: case 0xEC: v = RD(wx); t = X & mkx; r65_c = t >= v; x8 ? setnz8(t - v) : setnz16(t - v); break;
        /* ---- BIT */
        case 0x89: r65_z = A & v & mka; break;
        case 0x24: case 0x2C: case 0x34: case 0x3C:
            v = RD(wa); r65_z = A & v & mka;
            if (m8) { r65_n = (int8_t)v; r65_v = (v >> 6) & 1; } else { r65_n = (int16_t)v; r65_v = (v >> 14) & 1; }
            break;
        /* ---- TSB / TRB */
        case 0x04: case 0x0C: v = RD(wa); r65_z = A & v & mka; WR(wa, (v | A) & mka); break;
        case 0x14: case 0x1C: v = RD(wa); r65_z = A & v & mka; WR(wa, (v & ~A) & mka); break;
        /* ---- shifts / inc / dec (memory) */
        case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E: case 0x36: case 0x3E:
        case 0x46: case 0x4E: case 0x56: case 0x5E: case 0x66: case 0x6E: case 0x76: case 0x7E:
        case 0xC6: case 0xCE: case 0xD6: case 0xDE: case 0xE6: case 0xEE: case 0xF6: case 0xFE:
        case 0x0A: case 0x2A: case 0x4A: case 0x6A: case 0x1A: case 0x3A: {
            int acc = (op & 0x0F) == 0x0A || op == 0x1A || op == 0x3A;
            uint32_t sb = wa - 1;
            v = acc ? (A & mka) : RD(wa);
            switch (op & 0xE0) {
            case 0x00: if (acc && op == 0x1A) { t = (v + 1) & mka; break; } r65_c = (v >> sb) & 1; t = (v << 1) & mka; break;
            case 0x20: if (acc && op == 0x3A) { t = (v - 1) & mka; break; } t = ((v << 1) | r65_c) & mka; r65_c = (v >> sb) & 1; break;
            case 0x40: r65_c = v & 1; t = v >> 1; break;
            case 0x60: t = (v >> 1) | (r65_c << sb); r65_c = v & 1; break;
            case 0xC0: t = (v - 1) & mka; break;
            default: t = (v + 1) & mka; break;
            }
            if (acc) { if (m8) A = (A & 0xFF00) | t; else A = t; }
            else WR(wa, t);
            if (m8) setnz8(t); else setnz16(t);
            break; }
        /* ---- register ops */
        case 0xE8: X = (X + 1) & mkx; x8 ? setnz8(X) : setnz16(X); break;
        case 0xC8: Y = (Y + 1) & mkx; x8 ? setnz8(Y) : setnz16(Y); break;
        case 0xCA: X = (X - 1) & mkx; x8 ? setnz8(X) : setnz16(X); break;
        case 0x88: Y = (Y - 1) & mkx; x8 ? setnz8(Y) : setnz16(Y); break;
        case 0xAA: X = A & mkx; x8 ? setnz8(X) : setnz16(X); break;
        case 0xA8: Y = A & mkx; x8 ? setnz8(Y) : setnz16(Y); break;
        case 0x8A: if (m8) { A = (A & 0xFF00) | (X & 0xFF); setnz8(A); } else { A = X & 0xFFFF; setnz16(A); } break;
        case 0x98: if (m8) { A = (A & 0xFF00) | (Y & 0xFF); setnz8(A); } else { A = Y & 0xFFFF; setnz16(A); } break;
        case 0x9B: Y = X; x8 ? setnz8(Y) : setnz16(Y); break;
        case 0xBB: X = Y; x8 ? setnz8(X) : setnz16(X); break;
        case 0x9A: S = X; break;
        case 0xBA: X = S & mkx; x8 ? setnz8(X) : setnz16(X); break;
        case 0x1B: S = A; break;
        case 0x3B: A = S; setnz16(A); break;
        case 0x5B: D = A; setnz16(A); break;
        case 0x7B: A = D; setnz16(A); break;
        case 0xEB: A = ((A >> 8) | (A << 8)) & 0xFFFF; setnz8(A); break;
        /* ---- stack */
        case 0x48: if (!m8) PUSHB(A >> 8); PUSHB(A); break;
        case 0xDA: if (!x8) PUSHB(X >> 8); PUSHB(X); break;
        case 0x5A: if (!x8) PUSHB(Y >> 8); PUSHB(Y); break;
        case 0x68: { uint32_t l, h; PULLB(l); if (m8) { A = (A & 0xFF00) | l; setnz8(l); } else { PULLB(h); A = l | h << 8; setnz16(A); } break; }
        case 0xFA: { uint32_t l, h; PULLB(l); if (x8) { X = l; setnz8(l); } else { PULLB(h); X = l | h << 8; setnz16(X); } break; }
        case 0x7A: { uint32_t l, h; PULLB(l); if (x8) { Y = l; setnz8(l); } else { PULLB(h); Y = l | h << 8; setnz16(Y); } break; }
        case 0x8B: PUSHB(DB); break;
        case 0xAB: PULLB(DB); setnz8(DB); break;
        case 0x4B: PUSHB(pc >> 16); break;
        case 0x0B: PUSHB(D >> 8); PUSHB(D); break;
        case 0x2B: { uint32_t l, h; PULLB(l); PULLB(h); D = l | h << 8; setnz16(D); break; }
        case 0xF4: PUSHB(b2); PUSHB(b1); len = 3; break;
        case 0xD4: t = rd16_b0((D + b1) & 0xFFFF); PUSHB(t >> 8); PUSHB(t); len = 2; break;
        case 0x62: t = (pc + 3 + (int16_t)w) & 0xFFFF; PUSHB(t >> 8); PUSHB(t); len = 3; break;
        case 0x08: PUSHB(((r65_n < 0) << 7) | (r65_v << 6) | MX | (r65_fd << 3) | (r65_fi << 2) | ((r65_z == 0) << 1) | r65_c); break;
        case 0x28: {
            uint32_t p; PULLB(p);
            r65_n = (p & 0x80) ? -1 : 0; r65_v = (p >> 6) & 1; r65_fd = (p >> 3) & 1; r65_fi = (p >> 2) & 1;
            r65_z = (p & 2) ? 0 : 1; r65_c = p & 1; MX = p & 0x30;
            if (p & 0x10) { X &= 0xFF; Y &= 0xFF; }
            break; }
        /* ---- flags */
        case 0x18: r65_c = 0; break;
        case 0x38: r65_c = 1; break;
        case 0xB8: r65_v = 0; break;
        case 0xD8: r65_fd = 0; break;
        case 0xF8: r65_fd = 1; break;
        case 0x58: r65_fi = 0; break;
        case 0x78: r65_fi = 1; break;
        case 0xC2: case 0xE2: {
            int on = op == 0xE2;
            if (b1 & 0x80) r65_n = on ? -1 : 0;
            if (b1 & 0x40) r65_v = on;
            if (b1 & 0x08) r65_fd = on;
            if (b1 & 0x04) r65_fi = on;
            if (b1 & 0x02) r65_z = on ? 0 : 1;
            if (b1 & 0x01) r65_c = on;
            if (on) { MX |= b1 & 0x30; if (b1 & 0x10) { X &= 0xFF; Y &= 0xFF; } }
            else MX &= ~(b1 & 0x30);
            len = 2; break; }
        case 0xEA: case 0x42: len = op == 0x42 ? 2 : 1; break;
        /* ---- block moves */
        case 0x54: case 0x44:
            blockmove(b2, b1, op == 0x54 ? 1 : -1); DB = b1; len = 3; break;
        /* ---- branches */
        case 0x10: case 0x30: case 0x50: case 0x70: case 0x80: case 0x90: case 0xB0: case 0xD0: case 0xF0: {
            int take;
            switch (op) {
            case 0x10: take = r65_n >= 0; break;
            case 0x30: take = r65_n < 0; break;
            case 0x50: take = !r65_v; break;
            case 0x70: take = r65_v; break;
            case 0x90: take = !r65_c; break;
            case 0xB0: take = r65_c; break;
            case 0xD0: take = r65_z != 0; break;
            case 0xF0: take = r65_z == 0; break;
            default: take = 1;
            }
            pc = PCN(2);
            if (take) pc = pb | ((pc + (int8_t)b1) & 0xFFFF);
            continue; }
        case 0x82: pc = pb | ((pc + 3 + (int16_t)w) & 0xFFFF); continue;
        case 0x4C: pc = pb | w; continue;
        /* (I16) JML: hand off through the dispatcher (as JML [abs]) - a translated target runs native
           instead of the rest of the call staying in the interpreter (the sprite JML tables at BF8363) */
#ifndef R65_NO_JMLHANDOFF
        case 0x5C: return JUMPKEY | (MX << 20) | w | b3 << 16;
#else
        case 0x5C: pc = w | b3 << 16; continue;          /* (test builds: the old behaviour) */
#endif
        case 0x6C: return JUMPKEY | (MX << 20) | pb | mrd16(w);
        case 0x7C: return JUMPKEY | (MX << 20) | pb | mrd16(pb | ((w + X) & 0xFFFF));
        case 0xDC: return JUMPKEY | (MX << 20) | mrd16(w) | mrd8((w + 2) & 0xFFFF) << 16;
        case 0x20: case 0x22: case 0xFC: {
            uint32_t tgt, ret;
            if (op == 0x22) { tgt = w | b3 << 16; t = (pc + 3) & 0xFFFF; PUSHB(pc >> 16); PUSHB(t >> 8); PUSHB(t); ret = PCN(4); }
            else {
                tgt = op == 0x20 ? (pb | w) : (pb | mrd16(pb | ((w + X) & 0xFFFF)));
                t = (pc + 2) & 0xFFFF; PUSHB(t >> 8); PUSHB(t); ret = PCN(3);
            }
            uint32_t r = xrun((MX << 20) | tgt);
            if ((r & 0xFFFFFF) != ret || (r & (WAITKEY | ERRKEY))) return r;
            MX = (r >> 20) & 0x30;
            pc = ret;
            continue; }
        case 0x60: { uint32_t l, h; PULLB(l); PULLB(h); return (MX << 20) | pb | (((l | h << 8) + 1) & 0xFFFF); }
        case 0x6B: { uint32_t l, h, b; PULLB(l); PULLB(h); PULLB(b); return (MX << 20) | b << 16 | (((l | h << 8) + 1) & 0xFFFF); }
        case 0x40: {
            uint32_t p, l, h, b; PULLB(p);
            r65_n = (p & 0x80) ? -1 : 0; r65_v = (p >> 6) & 1; r65_fd = (p >> 3) & 1; r65_fi = (p >> 2) & 1;
            r65_z = (p & 2) ? 0 : 1; r65_c = p & 1; MX = p & 0x30;
            if (p & 0x10) { X &= 0xFF; Y &= 0xFF; }
            PULLB(l); PULLB(h); PULLB(b);
            return (MX << 20) | b << 16 | l | h << 8; }
        case 0xCB: return WAITKEY | (MX << 20) | PCN(1);
        default:
            return ERRKEY | pc;
        }
        pc = PCN(len);
    }
}
