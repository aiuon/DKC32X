#!/usr/bin/env python3
"""r65: static recompiler 65816 (DKC1 SNES code) -> C.

Inputs: the user's ROM, trace summaries from the instrumented snes9x (state.txt: executed PCs with
their M/X modes, DB and D values; edges.txt: indirect control transfers), and the DKC1 disassembly
(instruction boundaries, used to keep static discovery on real code).

Output: one C file with a giant function run65(cpu) (labels + gotos, computed-goto dispatch for
returns / indirect jumps) built on the macros of r65rt.h.

Mode index = (M << 1) | X with M/X = 1 meaning 8-bit (P bits 0x20 / 0x10).
"""
import sys, re, os, re, bisect, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ops65 import OPS, oplen
import disparse
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rompatch import WS_TUNE

def rom_off(a):
    return a & 0x3FFFFF

class Rom:
    def __init__(self, path):
        import rompatch
        self.d = rompatch.apply(open(path, 'rb').read())
    def b(self, a): return self.d[rom_off(a)]
    def w(self, a): return self.d[rom_off(a)] | self.d[rom_off(a + 1)] << 8
    def l(self, a): return self.w(a) | self.d[rom_off(a + 2)] << 16

def is_rom_addr(a):
    bank, off = a >> 16, a & 0xFFFF
    if bank in (0x7E, 0x7F): return False
    if bank & 0x40: return True
    return off >= 0x8000

def nextpc(pc, n):
    return (pc & 0xFF0000) | ((pc + n) & 0xFFFF)

# ---------------------------------------------------------------------------------------------
class Trace:
    def __init__(self, d):
        self.modes = collections.defaultdict(int)     # pc -> bitmask of modes
        self.db = {}                                  # pc -> set of db
        self.dp = {}                                  # pc -> set of d
        for ln in open(os.path.join(d, 'state.txt')):
            p = ln.split()
            pc = int(p[0], 16); pst = int(p[1], 16); db = int(p[2], 16); dp = int(p[3], 16)
            self.modes[pc] |= pst & 15
            s = self.db.setdefault(pc, set()); s.add(db & 0xFF)
            if db & 0x200: s.add(-1)
            s = self.dp.setdefault(pc, set()); s.add(dp & 0xFFFF)
            if dp & 0x20000: s.add(-1)
        self.edges = collections.defaultdict(set)
        for ln in open(os.path.join(d, 'edges.txt')):
            a, b = ln.split(); self.edges[int(a, 16)].add(int(b, 16))

# ---------------------------------------------------------------------------------------------
class Insn:
    __slots__ = ('pc', 'mode', 'op', 'mn', 'md', 'n', 'arg', 'succ', 'traced', 'targets', 'ret', 'pcall')

def decode(rom, pc, mode):
    i = Insn(); i.pc = pc; i.mode = mode
    i.op = rom.b(pc); i.mn, i.md = OPS[i.op]
    i.n = oplen(i.op, mode >> 1, mode & 1)
    i.arg = 0
    for k in range(i.n): i.arg |= rom.b(nextpc(pc, 1 + k)) << (8 * k)
    i.succ = []; i.targets = []; i.ret = None; i.pcall = False
    return i

class Analyzer:
    def __init__(self, rom, tr, dis, extra_entries=()):
        self.rom, self.tr, self.dis = rom, tr, dis
        self.insns = {}          # (pc, mode) -> Insn
        self.bad = set()
        self.dyn = set()         # (pc, mode) needing dispatch entries
        self.dyn_pcs = set()     # pcs needing dispatch for all their modes
        self.work = []
        for pc, ms in tr.modes.items():
            for m in range(4):
                if ms >> m & 1: self.add(pc, m, traced=True)
        for pc, m in extra_entries: self.add(pc, m)
        # dynamic transfers seen in the traces: returns to ordinary return sites are handled by the
        # call protocol; anything else (RTS/RTL tricks) becomes a function entry
        self.unusual = set()
        for src, ds in tr.edges.items():
            op = rom.b(src)
            if op == 0x40: continue          # RTI: interrupted code (lag frames), never resumed mid-code here
            for t in ds:
                if op in (0x60, 0x6B):
                    if rom.b(nextpc(t, -3)) in (0x20, 0xFC) or rom.b(nextpc(t, -4)) == 0x22: continue
                    self.unusual.add(t)

    def ok_static(self, pc, mode):
        """static (untraced) code must be a disassembly instruction with matching length"""
        if self.tr.modes.get(pc, 0) >> mode & 1: return True
        d = self.dis.get(pc)
        if d is None: return False
        op = self.rom.b(pc)
        if OPS[op][0] != d[0]: return False
        if oplen(op, mode >> 1, mode & 1) != d[1]: return False
        if OPS[op][0] in ('BRK', 'COP', 'WDM', 'STP'): return False
        return True

    def add(self, pc, mode, traced=False):
        k = (pc, mode)
        if k in self.insns or k in self.bad: return True
        if not traced and not self.ok_static(pc, mode):
            self.bad.add(k); return False
        i = decode(self.rom, pc, mode); i.traced = traced or bool(self.tr.modes.get(pc, 0) >> mode & 1)
        self.insns[k] = i; self.work.append(i)
        return True

    def run(self):
        while self.work:
            i = self.work.pop(); self.flow(i)

    def traced_modes(self, pc):
        ms = self.tr.modes.get(pc, 0)
        return [m for m in range(4) if ms >> m & 1]

    def flow(self, i):
        pc, mode, mn, md, arg = i.pc, i.mode, i.mn, i.md, i.arg
        nx = nextpc(pc, 1 + i.n)
        fall = True
        tgt = []
        if md == 'rel8':
            t = nextpc(nx, (arg ^ 0x80) - 0x80); tgt.append((t, mode))
            if mn == 'BRA': fall = False
        elif mn == 'BRL':
            t = nextpc(nx, (arg ^ 0x8000) - 0x8000); tgt.append((t, mode)); fall = False
        elif mn == 'JMP' and md == 'abs':
            tgt.append(((pc & 0xFF0000) | arg, mode)); fall = False
        elif mn == 'JML' and md == 'absl':
            tgt.append((arg, mode)); fall = False
        elif mn == 'JSR' and md == 'abs':
            tgt.append(((pc & 0xFF0000) | arg, mode)); i.ret = nx
        elif mn == 'JSL':
            tgt.append((arg, mode)); i.ret = nx
        elif mn in ('JMP', 'JML', 'JSR') and md in ('absi', 'absix', 'absil'):
            fall = (mn == 'JSR')
            if fall: i.ret = nx
            elif self.rom.b(nextpc(pc, -3)) == 0xF4 and nextpc(pc, -3) in self.dis:
                # PHK / PEA ret-1 / JMP [ptr]: a call that returns to ret (animation script callbacks)
                ret = (pc & 0xFF0000) | ((self.rom.w(nextpc(pc, -2)) + 1) & 0xFFFF)
                i.pcall = True; i.ret = ret
            for t in self.tr.edges.get(pc, ()):
                for m in (self.traced_modes(t) or [mode]): tgt.append((t, m))
            if md == 'absix' and arg >= 0x8000:
                for t in self.jump_table((pc & 0xFF0000) | arg):
                    tgt.append(((pc & 0xFF0000) | t, mode))
            for t, m in tgt: self.dyn.add((t, m))
        elif mn in ('RTS', 'RTL', 'RTI', 'STP', 'BRK', 'COP'):
            fall = False
        if mn == 'REP':
            mode = mode & ~(((arg >> 5) & 1) << 1 | ((arg >> 4) & 1))
        elif mn == 'SEP':
            mode = mode | (((arg >> 5) & 1) << 1 | ((arg >> 4) & 1))
        if mn == 'PLP' or mn == 'XCE':
            ms = self.traced_modes(nx)
            self.dyn_pcs.add(nx)
            for m in ms: self.add(nx, m)
            fall = False
        i.targets = []
        for t, m in tgt:
            if self.add(t, m): i.targets.append((t, m))
            else: i.targets.append((t, m, 'bad'))
        if i.ret is not None:
            self.dyn_pcs.add(i.ret)
            for m in set([mode] + self.traced_modes(i.ret)): self.add(i.ret, m)
        if fall and i.ret is None:
            if mn == 'WAI': self.dyn_pcs.add(nx)
            if self.add(nx, mode): i.succ = [(nx, mode)]
            else: i.succ = [(nx, mode, 'bad')]

    def jump_table(self, a):
        """static jump table at ROM address a: words until the next label / non-code entry"""
        out = []
        lab = self.dis_labels
        j = bisect.bisect_right(lab, a)
        end = lab[j] if j < len(lab) else a + 2
        end = min(end, a + 512)
        k = a
        while k + 1 < end:
            t = self.rom.w(k)
            if t < 0x8000 or ((a & 0xFF0000) | t) not in self.dis: break
            out.append(t); k += 2
        return out

# ---------------------------------------------------------------------------------------------
FN, FZ, FC, FV = 1, 2, 4, 8
FALL = 15
NZ_DEF = set('LDA LDX LDY TAX TAY TXA TYA TXY TYX TSX TSC TDC TCD PLA PLX PLY PLB PLD AND ORA EOR ADC SBC '
             'CMP CPX CPY INC DEC INX INY DEX DEY ASL LSR ROL ROR XBA'.split())

def flag_ud(i):
    """(uses, defs) flag bitmasks of an instruction (calls / returns handled by the caller)"""
    mn, md = i.mn, i.md
    u = d = 0
    if mn in NZ_DEF: d |= FN | FZ
    if mn in ('ADC', 'SBC', 'CMP', 'CPX', 'CPY', 'ASL', 'LSR', 'ROL', 'ROR', 'CLC', 'SEC'): d |= FC
    if mn in ('ADC', 'SBC', 'CLV'): d |= FV
    if mn in ('ADC', 'SBC', 'ROL', 'ROR'): u |= FC
    if mn == 'BIT': d |= FZ if md == 'immM' else (FN | FZ | FV)
    if mn in ('TSB', 'TRB'): d |= FZ
    if mn in ('BEQ', 'BNE'): u |= FZ
    if mn in ('BMI', 'BPL'): u |= FN
    if mn in ('BCC', 'BCS'): u |= FC
    if mn in ('BVC', 'BVS'): u |= FV
    if mn in ('REP', 'SEP'):
        if i.arg & 0x80: d |= FN
        if i.arg & 0x40: d |= FV
        if i.arg & 0x02: d |= FZ
        if i.arg & 0x01: d |= FC
    if mn in ('PLP', 'RTI'): d |= FALL
    if mn in ('PHP', 'WAI', 'BRK', 'COP', 'STP', 'XCE'): u |= FALL
    return u, d

def H(v, w=4): return '0x%0*X' % (w, v)

def wlw(a):
    """16-bit read of WRAM-lo at a constant address"""
    if a & 1: return '(WL(%s) | WL(%s) << 8)' % (H(a), H(a + 1))
    return 'WLW(%s)' % H(a)

def wlw_set(a, v):
    if a & 1: return '{ uint32_t _w = %s; WL(%s) = (uint8_t)_w; WL(%s) = (uint8_t)(_w >> 8); }' % (v, H(a), H(a + 1))
    return 'WLW_SET(%s, %s);' % (H(a), v)

class Emitter:
    def __init__(self, an, opts):
        self.an = an; self.rom = an.rom; self.opts = opts
        self.out = []
        self.lab_used = set()

    def L(self, pc, mode): return 'L%06X_%d' % (pc, mode)

    def e(self, s): self.out.append(s)

    # ---- trace-derived facts -------------------------------------------------------------
    def d_zero(self, pc):
        s = self.an.tr.dp.get(pc)
        return s is not None and s == {0}

    def db_class(self, pc):
        s = self.an.tr.db.get(pc)
        if not s or -1 in s and len(s) == 1: return None
        s = set(x for x in s if x >= 0)
        if all((b & 0x40) == 0 for b in s): return 'sys'
        return None

    # ---- effective address descriptors -----------------------------------------------------
    # returns (kind, expr): kind 'wc' const WRAM-lo address, 'b0' bank0 16-bit expr,
    # 'sys' (db, off) dynamic system bank off expr (may exceed 0xFFFF), 'l' 24-bit expr,
    # 'lc' 24-bit const
    def ea(self, i):
        md, arg, pc = i.md, i.arg, i.pc
        xw = 'x'; yw = 'y'
        dz = self.d_zero(pc)
        def dpe(k):
            return H(k) if dz else '((d + %s) & 0xFFFF)' % H(k)
        if md == 'dp':
            return ('wc', H(arg)) if dz else ('b0', dpe(arg))
        if md == 'dpx':
            return ('b0', '((%s + x) & 0xFFFF)' % (H(arg) if dz else '(d + %s)' % H(arg)))
        if md == 'dpy':
            return ('b0', '((%s + y) & 0xFFFF)' % (H(arg) if dz else '(d + %s)' % H(arg)))
        if md == 'dpi':
            return ('l', '((db << 16) | %s)' % self.rd16_b0(dpe(arg), dz and arg))
        if md == 'dpix':
            p = '((%s + x) & 0xFFFF)' % (H(arg) if dz else '(d + %s)' % H(arg))
            return ('l', '((db << 16) | RD16_B0(%s))' % p)
        if md == 'dpiy':
            return ('l', '((((db << 16) | %s) + y) & 0xFFFFFF)' % self.rd16_b0(dpe(arg), dz and arg))
        if md == 'dpil':
            return ('l', self.rd24_b0(dpe(arg), dz and arg))
        if md == 'dpily':
            return ('l', '((%s + y) & 0xFFFFFF)' % self.rd24_b0(dpe(arg), dz and arg))
        if md == 'sr':
            return ('b0', '((s + %s) & 0xFFFF)' % H(arg, 2))
        if md == 'sriy':
            return ('l', '((((db << 16) | RD16_B0((s + %s) & 0xFFFF)) + y) & 0xFFFFFF)' % H(arg, 2))
        cls = self.db_class(pc)
        if md == 'abs':
            if cls == 'sys':
                if arg < 0x2000: return ('wc', H(arg))
                return ('sys', H(arg))
            if arg < 0x2000 and os.environ.get('R65_DW', '1') != '0':
                return ('dw', H(arg))          # WRAM lo unless DB is $7F or a ROM bank (checked at run time)
            return ('l', '((db << 16) | %s)' % H(arg))
        if md in ('absx', 'absy'):
            r = 'x' if md == 'absx' else 'y'
            if cls == 'sys': return ('sys', '(%s + %s)' % (H(arg), r))
            if arg < 0x2000 and os.environ.get('R65_DW', '1') != '0':
                return ('dw', '(%s + %s)' % (H(arg), r))
            return ('l', '(((db << 16) + %s + %s) & 0xFFFFFF)' % (H(arg), r))
        if md == 'absl':
            return ('lc', arg)
        if md == 'abslx':
            return ('lx', arg)
        raise ValueError(md)

    def rd16_b0(self, e, const):
        if const is not False and const is not None and isinstance(const, int) and const < 0x1FFF:
            return wlw(const)
        return 'RD16_B0(%s)' % e

    def rd24_b0(self, e, const):
        if const is not False and const is not None and isinstance(const, int) and const < 0x1FFE:
            return '(WL(%s) | WL(%s) << 8 | WL(%s) << 16)' % (H(const), H(const + 1), H(const + 2))
        return 'RD24_B0(%s)' % e

    def rd(self, ea, w):
        k, e = ea
        if k == 'wc':
            a = int(e, 16)
            if w == 8: return 'WL(%s)' % e
            return wlw(a)
        if k == 'b0': return ('RD8_B0(%s)' if w == 8 else 'RD16_B0(%s)') % e
        if k == 'sys':
            m = re.match(r'\(?(0x[0-9A-F]+)', e)
            if m and int(m.group(1), 16) >= 0x8000:
                return ('RD8_SYSR(%s)' if w == 8 else 'RD16_SYSR(%s)') % e
            return ('RD8_SYS(%s)' if w == 8 else 'RD16_SYS(%s)') % e
        if k == 'l': return ('RD8(%s)' if w == 8 else 'RD16(%s)') % e
        if k == 'dw': return ('RD8_DW(%s)' if w == 8 else 'RD16_DW(%s)') % e
        if k == 'lc':
            a = e; bank, off = a >> 16, a & 0xFFFF
            if is_rom_addr(a):
                if w == 8: return 'ROM8(%s)' % H(rom_off(a), 6)
                return 'ROM16(%s)' % H(rom_off(a), 6)
            if bank in (0x7E, 0x7F) or off < 0x2000:
                wa = ((bank & 1) << 16 | off) if bank in (0x7E, 0x7F) else off
                if wa + 1 < 0x2000:
                    return 'WL(%s)' % H(wa) if w == 8 else wlw(wa)
                return 'WH(%s)' % H(wa, 5) if w == 8 else '(WH(%s) | WH(%s) << 8)' % (H(wa, 5), H(wa + 1, 5))
            return ('RD8(%s)' if w == 8 else 'RD16(%s)') % H(a, 6)
        if k == 'lx':
            a = e; bank, off = a >> 16, a & 0xFFFF
            if is_rom_addr(a) and (bank & 0x40):
                return ('ROM8(%s + x)' if w == 8 else 'ROM16(%s + x)') % H(rom_off(a), 6)
            if bank in (0x7E, 0x7F):
                return ('WH(%s + x)' if w == 8 else 'RD16_WH(%s + x)') % H(((bank & 1) << 16) | off, 5)
            return ('RD8(%s)' if w == 8 else 'RD16(%s)') % ('((%s + x) & 0xFFFFFF)' % H(a, 6))
        raise ValueError(k)

    def wr(self, ea, w, v):
        k, e = ea
        if k == 'wc':
            a = int(e, 16)
            if w == 8: return 'WL(%s) = (uint8_t)(%s);' % (e, v)
            return wlw_set(a, v)
        if k == 'b0': return ('WR8_B0(%s, %s);' if w == 8 else 'WR16_B0(%s, %s);') % (e, v)
        if k == 'sys': return ('WR8_SYS(%s, %s);' if w == 8 else 'WR16_SYS(%s, %s);') % (e, v)
        if k == 'l': return ('WR8(%s, %s);' if w == 8 else 'WR16(%s, %s);') % (e, v)
        if k == 'dw': return ('WR8_DW(%s, %s);' if w == 8 else 'WR16_DW(%s, %s);') % (e, v)
        if k == 'lc':
            a = e; bank, off = a >> 16, a & 0xFFFF
            if bank in (0x7E, 0x7F) or (not (bank & 0x40) and off < 0x2000):
                wa = ((bank & 1) << 16 | off) if bank in (0x7E, 0x7F) else off
                if wa + 1 < 0x2000:
                    if w == 8: return 'WL(%s) = (uint8_t)(%s);' % (H(wa), v)
                    return wlw_set(wa, v)
                if w == 8: return 'WH_SET(%s, %s);' % (H(wa, 5), v)
                return '{ uint32_t _w = %s; WH_SET(%s, _w); WH_SET(%s, _w >> 8); }' % (v, H(wa, 5), H(wa + 1, 5))
            return ('WR8(%s, %s);' if w == 8 else 'WR16(%s, %s);') % (H(a, 6), v)
        if k == 'lx':
            a = e; bank, off = a >> 16, a & 0xFFFF
            if bank in (0x7E, 0x7F):
                wa = ((bank & 1) << 16) | off
                if w == 8: return 'WH_SET(%s + x, %s);' % (H(wa, 5), v)
                return 'WR16_WH(%s + x, %s);' % (H(wa, 5), v)
            return ('WR8(%s, %s);' if w == 8 else 'WR16(%s, %s);') % ('((%s + x) & 0xFFFFFF)' % H(a, 6), v)
        raise ValueError(k)

    # ---- helpers ---------------------------------------------------------------------------
    def goto(self, t, m=None):
        """transfer to (pc, mode): local goto or tail call to another function"""
        if isinstance(t, tuple):
            if len(t) == 3: return 'BADJUMP(%s);' % H(t[0], 6)
            t, m = t
        k = (t, m)
        if k in self.fbody:
            self.lab_used.add(k)
            return 'goto %s;' % self.L(t, m)
        if k in self.tset:
            return 'return %s();' % self.F(k)
        return 'return JUMPKEY | %s;' % self.key(t, m)

    def F(self, k):
        h = self.opts.get('hle', {}).get(k)
        return h if h else 'F%06X_%d' % k

    def key(self, pc, m): return '0x%08X' % ((m << 24) | pc)

    def call(self, k, ret, mode, nb):
        """JSR (nb = 2) / JSL (3) to k, then continue at return site ret: the r65_call helpers push
        the return bytes, run the target and return 0 when it came back to ret (else the key to
        unwind with).  n / v locals are written back / reloaded only where the flag liveness needs it."""
        exp = self.expect(ret, mode)
        ek = self.key(ret, exp)
        lin = self.entry_live.get(k, FALL) if k in self.tset else FALL
        lout = self.cur_live
        so = 'db' + ('n' if (lin | lout) & FN else '') + ('v' if (lin | lout) & FV else '')
        si = 'db' + ('n' if lout & FN else '') + ('v' if lout & FV else '')
        if k in self.tset:
            c = 'r = r65_call%d(%s, %s);' % (nb, self.F(k), ek)
        else:
            c = 'r = r65_callx%d(%s, %s);' % (nb, self.key(*k), ek)
        return '@SO:%s@ %s if (r) return r; @SI:%s@ %s' % (so, c, si, self.goto((ret, exp)))

    def ret_sync(self):
        """write back at RTS / RTL: db always, n / v if a caller may read them"""
        x = self.exit_live.get(self.cur_func, FALL)
        return '@SO:db%s%s@' % ('n' if x & FN else '', 'v' if x & FV else '')

    def expect(self, ret, mode):
        if (ret, mode) in self.an.insns: return mode
        ms = self.an.traced_modes(ret)
        return ms[0] if ms else mode

    def nz(self, w, e):
        lv = self.cur_live
        o = []
        if lv & FZ: o.append('z = (%s) & %s;' % (e, '0xFF' if w == 8 else '0xFFFF'))
        if lv & FN: o.append('n = (%s)(%s);' % ('int8_t' if w == 8 else 'int16_t', e))
        return ' '.join(o)

    def cset(self, e):
        return ('c = %s;' % e) if self.cur_live & FC else ''

    def dispatch(self, mode_expr, pc_expr, kind='ljump'):
        return 'key = ((%s) << 20) | (%s); goto %s;' % (mode_expr, pc_expr, kind)

    # ---- instruction -----------------------------------------------------------------------
    def emit_insn(self, i, nxt_label):
        mn, md, arg, pc, mode = i.mn, i.md, i.arg, i.pc, i.mode
        M8 = mode >> 1 & 1; X8 = mode & 1
        wA = 8 if M8 else 16; wX = 8 if X8 else 16
        mA = 0xFF if M8 else 0xFFFF; mX = 0xFF if X8 else 0xFFFF
        nx = nextpc(pc, 1 + i.n)
        o = []
        E = o.append
        ms = '%d' % (mode << 4)    # runtime mx value for this mode
        def setA(v):   # assign low part of A per width
            if M8: return 'a = (a & 0xFF00) | ((%s) & 0xFF);' % v
            return 'a = (%s) & 0xFFFF;' % v
        def opnd(w):
            if md in ('immM', 'immX'):
                k = WS_TUNE.get(pc)
                if k is not None and md == 'immM' and not M8:     # (I21) OBJECT RANGE option
                    return '((%s + r65_ws[%d]) & 0xFFFF)' % (H(arg), k)
                return H(arg)
            return self.rd(self.ea(i), w)
        if mn in ('LDA', 'LDX', 'LDY'):
            if mn == 'LDA':
                E('{ uint32_t _m = %s; %s %s }' % (opnd(wA), setA('_m'), self.nz(wA, '_m')))
            else:
                r = mn[2].lower()
                E('%s = %s; %s' % (r, opnd(wX), self.nz(wX, r)))
        elif mn in ('STA', 'STX', 'STY', 'STZ'):
            if mn == 'STA': E(self.wr(self.ea(i), wA, 'a'))
            elif mn == 'STZ': E(self.wr(self.ea(i), wA, '0'))
            else: E(self.wr(self.ea(i), wX, mn[2].lower()))
        elif mn in ('ORA', 'AND', 'EOR'):
            opc = {'ORA': '|', 'AND': '&', 'EOR': '^'}[mn]
            E('{ uint32_t _r = (a %s %s) & %s; %s %s }' % (opc, opnd(wA), H(mA), setA('_r'), self.nz(wA, '_r')))
        elif mn in ('ADC', 'SBC'):
            lv = self.cur_live; mk = H(mA); sb = 7 if wA == 8 else 15
            m_ = opnd(wA) if mn == 'ADC' else '(%s) ^ %s' % (opnd(wA), mk)
            q = ['uint32_t _m = %s, _t = (a & %s) + _m + c;' % (m_, mk)]
            if lv & FV: q.append('v = ((~(a ^ _m) & (_m ^ _t)) >> %d) & 1;' % sb)
            if lv & FC: q.append('c = _t >> %d;' % (wA))
            q.append(setA('_t')); q.append(self.nz(wA, '_t'))
            E('{ ' + ' '.join(q) + ' }')
        elif mn in ('CMP', 'CPX', 'CPY'):
            r = {'CMP': 'a', 'CPX': 'x', 'CPY': 'y'}[mn]
            w = wA if mn == 'CMP' else wX; mk = 0xFF if w == 8 else 0xFFFF
            if self.cur_live & (FN | FZ | FC):
                E('{ uint32_t _m = %s, _r = %s & %s; %s %s }' % (opnd(w), r, H(mk), self.cset('_r >= _m'), self.nz(w, '_r - _m')))
        elif mn == 'BIT':
            if md == 'immM':
                if self.cur_live & FZ: E('z = a & %s & %s;' % (H(arg), H(mA)))
            else:
                lv = self.cur_live; q = []
                if lv & FZ: q.append('z = a & _m & %s;' % H(mA))
                if lv & FN: q.append('n = (%s)_m;' % ('int8_t' if wA == 8 else 'int16_t'))
                if lv & FV: q.append('v = (_m >> %d) & 1;' % (6 if wA == 8 else 14))
                if q: E('{ uint32_t _m = %s; %s }' % (opnd(wA), ' '.join(q)))
        elif mn in ('INC', 'DEC') and md == 'acc':
            d = '+ 1' if mn == 'INC' else '- 1'
            E('{ uint32_t _r = (a %s) & %s; %s %s }' % (d, H(mA), setA('_r'), self.nz(wA, '_r')))
        elif mn in ('INX', 'INY', 'DEX', 'DEY'):
            r = mn[2].lower(); d = '+ 1' if mn[0] == 'I' else '- 1'
            E('%s = (%s %s) & %s; %s' % (r, r, d, H(mX), self.nz(wX, r)))
        elif mn in ('ASL', 'LSR', 'ROL', 'ROR', 'INC', 'DEC', 'TSB', 'TRB'):
            w = wA; mk = mA; sb = 7 if w == 8 else 15
            def body(src):
                if mn == 'ASL': return '%s _r = (%s << 1) & %s;' % (self.cset('(%s >> %d) & 1' % (src, sb)), src, H(mk))
                if mn == 'LSR': return '%s _r = %s >> 1;' % (self.cset('%s & 1' % src), src)
                if mn == 'ROL': return '_r = ((%s << 1) | c) & %s; %s' % (src, H(mk), self.cset('(%s >> %d) & 1' % (src, sb)))
                if mn == 'ROR': return '_r = (%s >> 1) | (c << %d); %s' % (src, sb, self.cset('%s & 1' % src))
                if mn == 'INC': return '_r = (%s + 1) & %s;' % (src, H(mk))
                if mn == 'DEC': return '_r = (%s - 1) & %s;' % (src, H(mk))
            if md == 'acc':
                E('{ uint32_t _s = a & %s, _r; %s %s %s }' % (H(mk), body('_s'), setA('_r'), self.nz(w, '_r')))
            else:
                ea = self.ea(i)
                if ea[0] in ('b0', 'sys', 'l', 'dw'):
                    E('{ uint32_t _ea = %s;' % ea[1]); ea = (ea[0], '_ea')
                else:
                    E('{')
                if mn in ('TSB', 'TRB'):
                    op = '|' if mn == 'TSB' else '& ~'
                    E('uint32_t _s = %s, _r; z = a & _s & %s; _r = (_s %s a) & %s;' % (self.rd(ea, w), H(mk), op, H(mk)))
                    E(self.wr(ea, w, '_r') + ' }')
                else:
                    E('uint32_t _s = %s, _r; %s %s' % (self.rd(ea, w), body('_s'), self.nz(w, '_r')))
                    E(self.wr(ea, w, '_r') + ' }')
        elif mn in ('TAX', 'TAY'):
            r = mn[2].lower()
            E('%s = a & %s; %s' % (r, H(mX), self.nz(wX, r)))
        elif mn in ('TXA', 'TYA'):
            r = mn[1].lower()
            E('%s %s' % (setA(r), self.nz(wA, 'a')))
        elif mn == 'TXY': E('y = x; %s' % self.nz(wX, 'y'))
        elif mn == 'TYX': E('x = y; %s' % self.nz(wX, 'x'))
        elif mn == 'TXS': E('s = x;')
        elif mn == 'TSX': E('x = s & %s; %s' % (H(mX), self.nz(wX, 'x')))
        elif mn == 'TCS': E('s = a;')
        elif mn == 'TSC': E('a = s; ' + self.nz(16, 'a') + '')
        elif mn == 'TCD': E('d = a; ' + self.nz(16, 'a') + '')
        elif mn == 'TDC': E('a = d; ' + self.nz(16, 'a') + '')
        elif mn == 'XBA': E('a = ((a >> 8) | (a << 8)) & 0xFFFF; ' + self.nz(8, 'a') + '')
        elif mn == 'PHA': E('PUSH8(a >> 8); PUSH8(a);' if wA == 16 else 'PUSH8(a);')
        elif mn in ('PHX', 'PHY'):
            r = mn[2].lower(); E(('PUSH8(%s >> 8); PUSH8(%s);' % (r, r)) if wX == 16 else 'PUSH8(%s);' % r)
        elif mn == 'PLA':
            if wA == 16: E('{ uint32_t _l, _h; PULL8(_l); PULL8(_h); a = _l | _h << 8; ' + self.nz(16, 'a') + ' }')
            else: E('{ uint32_t _l; PULL8(_l); a = (a & 0xFF00) | _l; ' + self.nz(8, '_l') + ' }')
        elif mn in ('PLX', 'PLY'):
            r = mn[2].lower()
            if wX == 16: E('{ uint32_t _l, _h; PULL8(_l); PULL8(_h); %s = _l | _h << 8; %s }' % (r, self.nz(16, r)))
            else: E('{ uint32_t _l; PULL8(_l); %s = _l; %s }' % (r, self.nz(8, '_l')))
        elif mn == 'PHB': E('PUSH8(db);')
        elif mn == 'PLB': E('PULL8(db); ' + self.nz(8, 'db') + '')
        elif mn == 'PHK': E('PUSH8(%s);' % H(pc >> 16, 2))
        elif mn == 'PHD': E('PUSH8(d >> 8); PUSH8(d);')
        elif mn == 'PLD': E('{ uint32_t _l, _h; PULL8(_l); PULL8(_h); d = _l | _h << 8; ' + self.nz(16, 'd') + ' }')
        elif mn == 'PEA': E('PUSH8(%s); PUSH8(%s);' % (H(arg >> 8, 2), H(arg & 0xFF, 2)))
        elif mn == 'PEI':
            dz = self.d_zero(pc)
            e = self.rd16_b0(H(arg) if dz else '((d + %s) & 0xFFFF)' % H(arg), dz and arg)
            E('{ uint32_t _w = %s; PUSH8(_w >> 8); PUSH8(_w); }' % e)
        elif mn == 'PER':
            t = (nx + ((arg ^ 0x8000) - 0x8000)) & 0xFFFF
            E('PUSH8(%s); PUSH8(%s);' % (H(t >> 8, 2), H(t & 0xFF, 2)))
        elif mn == 'PHP': E('PUSH8(PACKP(%s));' % ms)
        elif mn == 'PLP':
            q = ''
            for m in range(4):
                if (nx, m) in self.fbody:
                    q += 'if (mx == %d) goto %s; ' % (m << 4, self.L(nx, m))
            E('{ uint32_t _p; PULL8(_p); UNPACKP(_p); mx = _p & 0x30; %sreturn JUMPKEY | (mx << 20) | %s; }' % (q, H(nx, 6)))
            return o
        elif mn == 'CLC': E(self.cset('0'))
        elif mn == 'SEC': E(self.cset('1'))
        elif mn == 'CLV': E('v = 0;')
        elif mn == 'CLD': E('fd = 0;')
        elif mn == 'SED': E('fd = 1;')
        elif mn == 'CLI': E('fi = 0;')
        elif mn == 'SEI': E('fi = 1;')
        elif mn in ('REP', 'SEP'):
            s = []
            on = mn == 'SEP'
            lv = self.cur_live
            if arg & 0x80 and lv & FN: s.append('n = %s;' % ('-1' if on else '0'))
            if arg & 0x40 and lv & FV: s.append('v = %d;' % on)
            if arg & 0x08: s.append('fd = %d;' % on)
            if arg & 0x04: s.append('fi = %d;' % on)
            if arg & 0x02 and lv & FZ: s.append('z = %d;' % (0 if on else 1))
            if arg & 0x01 and lv & FC: s.append('c = %d;' % on)
            if on and (arg & 0x10): s.append('x &= 0xFF; y &= 0xFF;')
            E(' '.join(s))
        elif mn == 'NOP': pass
        elif mn == 'WDM': pass
        elif mn == 'XCE':
            E('XCE();')
            return o
        elif mn in ('MVN', 'MVP'):
            dst, src = arg & 0xFF, arg >> 8
            E('BLOCKMOVE(%s, %s, %s);' % (H(src, 2), H(dst, 2), '1' if mn == 'MVN' else '-1'))
        elif md == 'rel8' or mn == 'BRL':
            cond = {'BPL': 'n >= 0', 'BMI': 'n < 0', 'BVC': '!v', 'BVS': 'v', 'BCC': '!c', 'BCS': 'c',
                    'BNE': 'z', 'BEQ': '!z', 'BRA': None, 'BRL': None}[mn]
            t = i.targets[0]
            if cond: E('if (%s) %s' % (cond, self.goto(t)))
            else: E(self.goto(t)); return o
        elif mn in ('JMP', 'JML') and md in ('abs', 'absl'):
            E(self.goto(i.targets[0])); return o
        elif mn == 'JSR' and md == 'abs':
            r = (pc + 2) & 0xFFFF
            t = i.targets[0]
            if len(t) == 3: E('BADJUMP(%s);' % H(t[0], 6)); return o
            E(self.call(t, nx, mode, 2))
            return o
        elif mn == 'JSL':
            r = (pc + 3) & 0xFFFF
            t = i.targets[0]
            if len(t) == 3: E('BADJUMP(%s);' % H(t[0], 6)); return o
            E(self.call(t, nx, mode, 3))
            return o
        elif mn in ('JMP', 'JSR') and md == 'absix':
            pb = pc >> 16
            if mn == 'JSR':
                r = (pc + 2) & 0xFFFF
                exp = self.expect(nx, mode)
                E('{ uint32_t _t = RD16(%s | ((%s + x) & 0xFFFF)); PUSH8(%s); PUSH8(%s); r = xrun(%s | %s | _t); '
                  'if (r != %s) return r; %s }' % (H(pb << 16, 6), H(arg), H(r >> 8, 2), H(r & 0xFF, 2),
                  self.key(0, mode), H(pb << 16, 6), self.key(nx, exp), self.goto((nx, exp))))
            else:
                E('{ uint32_t _t = RD16(%s | ((%s + x) & 0xFFFF)); return JUMPKEY | %s | %s | _t; }' % (
                  H(pb << 16, 6), H(arg), self.key(0, mode), H(pb << 16, 6)))
            return o
        elif mn == 'JMP' and md == 'absi':
            # (I13) the pointer is in bank 0: below $1FFF it is WRAM (no mrd16 call for the jump tables
            # through direct-page / WRAM vectors)
            rd = '(WL(%s) | WL(%s) << 8)' % (H(arg), H(arg + 1)) if arg < 0x1FFF else 'RD16(%s)' % H(arg)
            E('{ uint32_t _t = %s; return JUMPKEY | %s | %s | _t; }' % (rd, self.key(0, mode), H(pc & 0xFF0000, 6)))
            return o
        elif mn == 'JML' and md == 'absil':
            if arg < 0x1FFE:
                rd = '(WL(%s) | WL(%s) << 8 | WL(%s) << 16)' % (H(arg), H(arg + 1), H(arg + 2))
            else:
                rd = 'RD16(%s) | RD8(%s) << 16' % (H(arg), H(arg + 2))
            if getattr(i, 'pcall', False):
                exp = self.expect(i.ret, mode)
                E('{ uint32_t _t = %s; r = xrun(%s | _t); if (r != %s) return r; %s }' % (
                  rd, self.key(0, mode), self.key(i.ret, exp), self.goto((i.ret, exp))))
            else:
                E('{ uint32_t _t = %s; return JUMPKEY | %s | _t; }' % (rd, self.key(0, mode)))
            return o
        elif mn == 'RTS':
            E('%s return /*ns*/ r65_rts(%s);' % (self.ret_sync(), '0x%08X' % ((mode << 24) | (pc & 0xFF0000))))
            return o
        elif mn == 'RTL':
            E('%s return /*ns*/ r65_rtl(%s);' % (self.ret_sync(), self.key(0, mode)))
            return o
        elif mn == 'RTI':
            E('{ uint32_t _p, _l, _h, _b; PULL8(_p); UNPACKP(_p); mx = _p & 0x30; PULL8(_l); PULL8(_h); PULL8(_b); '
              'return ((_p & 0x30) << 20) | (_b << 16) | _l | _h << 8; }')
            return o
        elif mn == 'WAI':
            E('return WAITKEY | %s;' % self.key(nx, mode)); return o
        elif mn in ('STP', 'BRK', 'COP'):
            E('return ERRKEY | %s;' % H(pc, 6)); return o
        else:
            raise ValueError('unhandled %s %s' % (mn, md))
        # fallthrough
        if i.ret is not None: return o
        if i.succ:
            s = i.succ[0]
            if len(s) == 3: E('BADJUMP(%s);' % H(s[0], 6))
            elif self.L(*s) != nxt_label: E(self.goto(s))
        else:
            E('BADJUMP(%s);' % H(nx, 6))
        return o

    def run(self, nfiles=16, translate=None):
        an = self.an
        # callable entries: direct call targets, indirect call / jump targets, unusual dynamic
        # targets (RTS/RTL tricks), data-referenced code, the vectors
        ents = set()
        for i in an.insns.values():
            if (i.mn in ('JSR', 'JSL') and i.md in ('abs', 'absl')) or getattr(i, 'pcall', False):
                for t in i.targets:
                    if len(t) == 2 and i.mn in ('JSR', 'JSL'): ents.add(t)
        ents |= set(k for k in an.dyn if k in an.insns)
        ents |= set(k for k in self.opts.get('entries', []) if k in an.insns)
        hle = self.opts.get('hle', {})
        ents |= set(k for k in hle if k in an.insns)
        for pc in an.unusual:
            for m in range(4):
                if (pc, m) in an.insns: ents.add((pc, m))
        def local_succ(i):
            out = [t for t in i.succ + [t for t in i.targets if i.mn not in ('JSR', 'JSL') and not getattr(i, 'pcall', False)] if len(t) == 2]
            if i.ret is not None:
                out.append((i.ret, self.expect(i.ret, i.mode)))
            if i.mn == 'PLP':
                nx = nextpc(i.pc, 1)
                for m in an.traced_modes(nx):
                    out.append((nx, m))
            return out
        def bodies():
            funcs = {}; owner = {}
            for e in sorted(ents):
                body = set(); work = [e]
                while work:
                    k = work.pop()
                    if k in body or k not in an.insns: continue
                    if k != e and k in ents: continue
                    body.add(k)
                    work.extend(local_succ(an.insns[k]))
                funcs[e] = body
                for k in body: owner.setdefault(k, e)
            return funcs, owner
        funcs, owner = bodies()
        # hot / cold split of the SDRAM functions: code the gameplay profile never ran moves to
        # separate (ROM) functions, entered by a tail call where the hot code branches into it, so
        # the SDRAM holds only hot code
        prof = self.opts.get('prof'); sd = self.opts.get('sdram', ())
        self.split = set()
        if prof and sd and os.environ.get('R65_SPLIT', '1') != '0':
            thr = float(os.environ.get('R65_SPLIT_T', '0'))      # runs per frame at or below: cold
            hot = lambda pc: prof.get(pc, 0) > thr
            for e in sorted(sd):
                if e not in funcs or e in hle: continue
                body = funcs[e]
                for k in body:
                    if not hot(k[0]): continue
                    for t in local_succ(an.insns[k]):
                        if t in body and not hot(t[0]) and t != e and t in an.insns:
                            self.split.add(t)
            ents |= self.split
            funcs, owner = bodies()
            print('split: %d cold entries out of the SDRAM functions' % len(self.split))
        tset = set(ents) if translate is None else (set(translate) & ents)
        tset |= set(k for k in hle if k in ents)
        tset |= self.split
        self.entries = ents
        self.tset = tset
        self.owner = owner
        # liveness over the translated functions; entries called from interpreted code see all flags
        forced = set()
        for e, body in funcs.items():
            if e in tset: continue
            for k in body:
                i = an.insns[k]
                for t in i.targets:
                    if len(t) == 2: forced.add(t)
        self.liveness(dict((e, funcs[e]) for e in tset if e not in hle), owner, forced)
        total = sum(len(funcs[e]) for e in tset if e not in hle)
        # emit
        hdr = ['/* generated by tools/recomp/r65.py - do not edit, do not distribute */', '#define R65_GEN', '#include "r65rt.h"']
        for e in sorted(set(self.F(e) for e in tset)): hdr.append('uint32_t %s(void);' % e)
        files = [[] for _ in range(nfiles)]
        sizes = [0] * nfiles
        for e in sorted((e for e in tset if e not in hle), key=lambda e: -len(funcs[e])):
            j = sizes.index(min(sizes)); sizes[j] += len(funcs[e])
            files[j].append(e)
        srcs = []
        for j in range(nfiles):
            o = ['/* generated by tools/recomp/r65.py from the DKC1 65816 code - do not edit, do not distribute */',
                 '#include "game65.h"']
            for e in sorted(files[j]):
                o.extend(self.emit_func(e, funcs[e]))
            srcs.append('\n'.join(o) + '\n')
        # dispatch table: translated entries
        dyn = sorted(tset, key=lambda k: (k[1] << 24) | k[0])
        t = ['/* generated by tools/recomp/r65.py - do not edit, do not distribute */', '#include "game65.h"',
             'const uint32_t r65_dkeys[%d] = {' % max(1, len(dyn))]
        for j in range(0, len(dyn), 8):
            t.append('    ' + ', '.join(self.key(pc, m) for pc, m in dyn[j:j + 8]) + ',')
        t.append('};')
        t.append('uint32_t (*const r65_dfuncs[%d])(void) = {' % max(1, len(dyn)))
        for j in range(0, len(dyn), 6):
            t.append('    ' + ', '.join(self.F(k) for k in dyn[j:j + 6]) + ',')
        t.append('};')
        t.append('const int r65_ndkeys = %d;' % len(dyn))
        srcs.append('\n'.join(t) + '\n')
        self.stats = (len(an.insns), len(ents), len(tset), total, len(dyn))
        if os.environ.get('R65_LEAFSTATS'):
            prof = self.opts.get('prof') or {}
            STK = set('PHA PHX PHY PHB PHD PHK PHP PLA PLX PLY PLB PLD PLP TSC TSX TXS TCS RTI RTL BRK COP WAI STP XCE PEA PEI PER JSL JSR JML JMP'.split())
            calls = collections.Counter()
            for i in an.insns.values():
                if i.mn == 'JSR' and i.md == 'abs' and i.targets and len(i.targets[0]) == 2:
                    calls[i.targets[0]] += prof.get(i.pc, 0)
            rows = []
            for e in tset:
                if e in hle: continue
                b = funcs[e]
                ok = all(an.insns[k].mn not in STK and an.insns[k].md not in ('sr', 'sriy') for k in b)
                ok = ok and all(not (set(t[:2] for t in an.insns[k].targets if len(t) >= 2) - b) or an.insns[k].mn == 'RTS' for k in b if an.insns[k].mn not in ('RTS',))
                rets = [k for k in b if an.insns[k].mn == 'RTS']
                if ok and rets: rows.append((calls[e], len(b), e))
            rows.sort(reverse=True)
            tot = sum(calls.values())
            print('leafstats: JSR calls/frame %.0f, leaf callees %d, calls to leaves %.0f' % (tot, len(rows), sum(r[0] for r in rows)))
            for c, n, e in rows[:40]: print('  leaf %06X m%d  %6.1f calls/frame  %3d insns' % (e[0], e[1], c, n))
        self.funcs = funcs
        return '\n'.join(hdr) + '\n', srcs

    def succ_edges(self, e, body, i):
        """CFG edges of instruction i inside function e: list of ('loc', k) / ('ent', T) / ('dyn',) /
        ('exit',) / ('call', T, retk) / ('dcall', targets, retk)"""
        an = self.an
        mn, md = i.mn, i.md
        out = []
        def tgt(t):
            if len(t) == 3: return None
            if t in body: return ('loc', t)
            if t in self.tset: return ('ent', t)
            return ('dyn',)
        if mn in ('RTS', 'RTL', 'RTI'): return [('exit',)]
        if mn in ('WAI', 'BRK', 'COP', 'STP'): return []
        if mn in ('PLP', 'XCE'): return [('dyn',)]
        if mn in ('JSR', 'JSL') and md in ('abs', 'absl'):
            t = i.targets[0]
            if len(t) == 3: return []
            r = (i.ret, self.expect(i.ret, i.mode))
            if t not in self.tset: return [('dcall', [t], r)]
            return [('call', t, r)]
        if getattr(i, 'pcall', False):
            r = (i.ret, self.expect(i.ret, i.mode))
            return [('dcall', [t for t in i.targets if len(t) == 2], r)]
        if mn == 'JSR' and md == 'absix':
            r = (i.ret, self.expect(i.ret, i.mode))
            return [('dcall', [t for t in i.targets if len(t) == 2], r)]
        if mn in ('JMP', 'JML') and md in ('absi', 'absix', 'absil'):
            return [('dyn',)]
        for t in i.succ + list(i.targets):
            x = tgt(t)
            if x: out.append(x)
        return out

    def liveness(self, funcs, owner, forced):
        an = self.an
        known = set()
        for e, body in funcs.items():
            for k in body:
                i = an.insns[k]
                for ed in self.succ_edges(e, body, i):
                    if ed[0] in ('call', 'ent'): known.add(ed[1])
        dynamic = set()
        for pc in an.dyn_pcs:
            for m in range(4): dynamic.add((pc, m))
        dynamic |= set(an.dyn)
        entry_live = dict((e, 0) for e in funcs)
        exit_live = dict((e, 0 if (e in known and e not in dynamic and e not in forced) else FALL) for e in funcs)
        self.live = {}
        ud = {}
        edges = {}
        for e, body in funcs.items():
            for k in body:
                if k not in ud: ud[k] = flag_ud(an.insns[k])
                edges[(e, k)] = self.succ_edges(e, body, an.insns[k])
        it = 0
        while True:
            it += 1
            changed = False
            for e, body in funcs.items():
                lin = dict((k, 0) for k in body)
                order = sorted(body, key=lambda k: -k[0])
                while True:
                    ch = False
                    for k in order:
                        u, d = ud[k]
                        lo = 0; call_in = None
                        for ed in edges[(e, k)]:
                            t = ed[0]
                            if t == 'loc': lo |= lin[ed[1]]
                            elif t == 'ent': lo |= entry_live.get(ed[1], FALL)
                            elif t == 'dyn': lo |= FALL
                            elif t == 'exit': lo |= exit_live[e]
                            elif t == 'call':
                                r = lin.get(ed[2], FALL) if ed[2] in body else entry_live.get(ed[2], FALL)
                                call_in = entry_live.get(ed[1], FALL) | r
                            elif t == 'dcall':
                                call_in = FALL
                        if call_in is not None: v = call_in
                        else: v = u | (lo & ~d)
                        if v != lin[k]: lin[k] = v; ch = True
                    if not ch: break
                # live-after per instruction (for emission)
                for k in body:
                    lo = 0
                    for ed in edges[(e, k)]:
                        t = ed[0]
                        if t == 'loc': lo |= lin[ed[1]]
                        elif t == 'ent': lo |= entry_live.get(ed[1], FALL)
                        elif t in ('dyn', 'dcall'): lo |= FALL
                        elif t == 'exit': lo |= exit_live[e]
                        elif t == 'call':
                            lo |= lin.get(ed[2], FALL) if ed[2] in body else FALL
                    self.live[(e, k)] = lo
                if lin[e] != entry_live[e]:
                    entry_live[e] = lin[e]; changed = True
                # propagate exit liveness to callees / tail targets
                for k in body:
                    for ed in edges[(e, k)]:
                        t = ed[0]
                        if t == 'call':
                            r = lin.get(ed[2], FALL) if ed[2] in body else FALL
                            T = ed[1]
                            if T in exit_live and exit_live[T] | r != exit_live[T]:
                                exit_live[T] |= r; changed = True
                        elif t == 'dcall':
                            r = lin.get(ed[2], FALL) if ed[2] in body else FALL
                            for tt in ed[1]:
                                T = owner.get(tt)
                                if T in exit_live and exit_live[T] | r != exit_live[T]:
                                    exit_live[T] |= r; changed = True
                        elif t == 'ent':
                            T = ed[1]
                            if T in exit_live and exit_live[T] | exit_live[e] != exit_live[T]:
                                exit_live[T] |= exit_live[e]; changed = True
                        elif t == 'dyn' and an.insns[k].mn in ('JMP', 'JML'):
                            for tt in an.insns[k].targets:
                                T = owner.get(tt[:2])
                                if T in exit_live and exit_live[T] | exit_live[e] != exit_live[T]:
                                    exit_live[T] |= exit_live[e]; changed = True
            if not changed: break
        self.entry_live, self.exit_live = entry_live, exit_live
        tot = sum(1 for v in self.live.values() if v != FALL)
        print('liveness: %d iterations, %d/%d insns with dead flags' % (it, tot, len(self.live)))

    def emit_func(self, e, body):
        self.fbody = body
        self.cur_func = e
        an = self.an
        order = sorted(body, key=lambda k: (k[1], k[0]))
        attr = 'SDRAM_FN ' if e in self.opts.get('sdram', ()) else ''
        if not attr and e in self.opts.get('lowrom', ()): attr = 'LOWROM_FN '
        if not attr and self.opts.get('hot') is not None and e not in self.opts['hot']: attr = 'COLD_FN '
        o = ['%suint32_t %s(void)' % (attr, self.F(e)), '{', '    uint32_t r; FENTER(%s);' % self.key(*e)]
        if order[0] != e: o.append('    goto %s;' % self.L(*e))
        for idx, k in enumerate(order):
            i = an.insns[k]
            nxt = self.L(*order[idx + 1]) if idx + 1 < len(order) else None
            self.cur_live = self.live.get((e, k), FALL)
            lines = self.emit_insn(i, nxt)
            dis = an.dis.get(i.pc)
            cm = ' /* %s */' % dis[3].replace('*/', '').replace('??', '? ?') if dis else ' /* %s */' % i.mn
            prof = self.opts.get('prof')
            cold = ' __attribute__((cold));' if prof is not None and not attr and i.pc not in prof else ''
            o.append('%s:%s INSN(0x%06X, %d);%s' % (self.L(i.pc, i.mode), cold, i.pc, i.mode, cm))
            for ln in lines: o.append('    ' + ln)
        o.append('    return ERRKEY | 0xFFFFFF;')
        o.append('}')
        prof = self.opts.get('prof')
        if prof:
            pcs = set(k[0] for k in body)
            ex = [prof[pc] for pc in pcs if pc in prof]
            if ex and sum(ex) / len(ex) >= 2.0:
                # a loop-heavy function: inline memory accessors (the code stays in the cache)
                o = [re.sub(r'\b(RD8_B0|RD16_B0|RD24_B0|WR8_B0|WR16_B0|RD8_SYS|RD16_SYS|RD8_SYSR|RD16_SYSR|WR8_SYS|WR16_SYS|RD8|RD16)\(', r'\1_I(', ln) for ln in o]
        return localize(o)

# ---- n / v / db as function locals (byte stores to WRAM alias every global: GCC reloads globals
# after each one).  Locals are written back before calls and returns and reloaded after calls.
_MACROS = None
def _macro_vars():
    global _MACROS
    if _MACROS is None:
        txt = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'r65rt.h')).read().replace('\\\n', ' ')
        defs = {}
        for m in re.finditer(r'^#define ([A-Za-z_]\w*)\(([^)]*)\)(.*)$', txt, re.M):
            params = set(p.strip() for p in m.group(2).split(','))
            defs.setdefault(m.group(1), set()).update(set(re.findall(r'[A-Za-z_]\w*', m.group(3))) - params)
        res = {}
        def clo(name, seen):
            if name in res: return res[name]
            out = set()
            for t in defs.get(name, ()):
                if t in LOCAL_VARS: out.add(t)
                elif t in defs and t not in seen: out |= clo(t, seen | {name})
            res[name] = out
            return out
        for k in defs: clo(k, set())
        _MACROS = res
    return _MACROS

LOCAL_VARS = ('n', 'v', 'db')
LOCAL_DECL = {'n': 'int32_t l_n = r65_n;', 'v': 'uint32_t l_v = r65_v;', 'db': 'uint32_t l_db = r65_db;'}
_CALL_RE = re.compile(r'(r = (?:\w+\(\); if \(r & JUMPKEY\) r = xrun\(r\);|xrun\([^;]*\);))')
_RET_RE = re.compile(r'\breturn ([^;]*);')
_PH_RE = re.compile(r'@(SO|SI):([a-z]*)@')

def localize(o):
    mac = _macro_vars()
    used = set()
    for ln in o[2:]:
        for t in re.findall(r'[A-Za-z_]\w*', re.sub(r'/\*.*?\*/', '', ln)):
            if t in LOCAL_VARS: used.add(t)
            elif t in mac: used |= mac[t]
    vs = [v for v in LOCAL_VARS if v in used]
    if not vs: return [_PH_RE.sub('', ln) for ln in o]
    so = ', '.join('r65_%s = l_%s' % (v, v) for v in vs)
    si = ' '.join('l_%s = r65_%s;' % (v, v) for v in vs)
    def ph(m):
        want = m.group(2)
        sel = [v for v in vs if (v == 'db' and 'db' in want) or (v in ('n', 'v') and v in want.replace('db', ''))]
        if m.group(1) == 'SO': return ' '.join('r65_%s = l_%s;' % (v, v) for v in sel)
        return ' '.join('l_%s = r65_%s;' % (v, v) for v in sel)
    out = []
    for idx, ln in enumerate(o):
        if idx == 2:
            out.append(ln + ' ' + ' '.join(LOCAL_DECL[v] for v in vs)); continue
        if ln.startswith('    ') and idx > 2:
            if '@S' not in ln:
                ln = _CALL_RE.sub(lambda m: '%s; %s %s' % (so, m.group(1), si), ln)
            ln = _RET_RE.sub(lambda m: m.group(0) if m.group(1) == 'r' or m.group(1).startswith('/*ns*/') else 'return (%s, (%s));' % (so, m.group(1)), ln)
            ln = _PH_RE.sub(ph, ln)
        out.append(ln)
    return out

def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('rom'); ap.add_argument('tracedir'); ap.add_argument('out')
    ap.add_argument('--translate', help='file with function entry keys to translate (default: all)')
    ap.add_argument('--files', type=int, default=16)
    ap.add_argument('--sdram', help='file with function keys to place in SDRAM (.sdtext)')
    ap.add_argument('--prof', help='per-PC execution counts (gameplay): never-run code in hot functions gets cold labels')
    ap.add_argument('--hot', help='file with the function keys that run often (the rest: COLD_FN = -Os)')
    ap.add_argument('--lowrom', help='(I19) file with function keys placed in the first 256 KB of the ROM (.lowtext, -Os)')
    a = ap.parse_args()
    rom = Rom(a.rom)
    tr = Trace(a.tracedir)
    _raw = open(a.rom, 'rb').read()
    insns, labels, named, refs = disparse.parse(rom=_raw[0x200:] if len(_raw) % 0x8000 == 0x200 else _raw)
    import rompatch
    rompatch.dis_fix(insns)            # (I21d) the patched instructions' boundaries
    an = Analyzer.__new__(Analyzer)
    an.dis_labels = sorted(labels.values())
    Analyzer.__init__(an, rom, tr, insns)
    an.run()
    # code referenced from data (animation scripts, pointer tables): dispatchable entries, mode
    # inferred from the disassembly's immediate sizes along the first straight-line run
    nref = 0
    for pc in sorted(refs):
        if an.tr.modes.get(pc): continue
        if pc not in insns: continue
        for m in (0, 2, 1, 3):
            k, okm = pc, True
            for _ in range(24):
                if not an.ok_static(k, m): okm = False; break
                op = rom.b(k); mn = OPS[op][0]
                if mn in ('REP', 'SEP', 'RTS', 'RTL', 'RTI', 'JMP', 'JML', 'BRA', 'BRL', 'PLP'): break
                k = nextpc(k, 1 + oplen(op, m >> 1, m & 1))
            if okm:
                if an.add(pc, m): an.dyn.add((pc, m)); nref += 1
                break
    an.run()
    print('%d data-referenced entries added' % nref)
    # entries: NMI / IRQ vectors in all modes
    ents = []
    for pc in (0x00A968, 0x00A990):
        for m in range(4):
            if an.add(pc, m, traced=True): ents.append((pc, m))
    an.run()
    # extra entries (data/entries.txt: KEY = mode << 24 | pc): code no trace reached that runs in the
    # interpreter (I11: the credits' sprite code, found with sim_xc XC_PROF)
    ep = os.path.join(a.tracedir, 'entries.txt')
    if os.path.exists(ep):
        for ln in open(ep):
            ln = ln.split('#')[0].strip()
            if not ln: continue
            k = int(ln.split()[0], 16); pc, m = k & 0xFFFFFF, k >> 24
            if an.add(pc, m, traced=True): an.dyn.add((pc, m))
        an.run()
    hle = {}
    hp = os.path.join(a.tracedir, 'hle.txt')
    if os.path.exists(hp):
        for ln in open(hp):
            ln = ln.split('#')[0].split()
            if len(ln) in (2, 3):
                pc = int(ln[0], 16)
                for m in range(4):
                    if len(ln) == 3 and m != int(ln[2]): continue     # optional: only this mode (M/X bits >> 4)
                    if (pc, m) in an.insns or (m == 0 and an.add(pc, m, traced=True)): hle[(pc, m)] = ln[1]
        an.run()
    sd = set()
    if a.sdram and os.path.exists(a.sdram):
        for ln in open(a.sdram):
            ln = ln.split('#')[0].strip()
            if ln: k = int(ln.split()[0], 16); sd.add((k & 0xFFFFFF, k >> 24))
    low = set()
    if a.lowrom and os.path.exists(a.lowrom):
        for ln in open(a.lowrom):
            ln = ln.split('#')[0].strip()
            if ln: k = int(ln.split()[0], 16); low.add((k & 0xFFFFFF, k >> 24))
    hot = None
    if a.hot and os.path.exists(a.hot):
        hot = set()
        for ln in open(a.hot):
            ln = ln.split('#')[0].strip()
            if ln: k = int(ln.split()[0], 16); hot.add((k & 0xFFFFFF, k >> 24))
    prof = None
    if a.prof and os.path.exists(a.prof):
        prof = {}
        nfr = 1
        for ln in open(a.prof):
            m = re.search(r'frames (\d+)', ln)
            if ln.startswith('#') and m: nfr = int(m.group(1))
            q = ln.split('#')[0].split()
            if len(q) == 2 and int(q[1]): prof[int(q[0], 16)] = int(q[1]) / nfr
    em = Emitter(an, {'entries': ents, 'hle': hle, 'sdram': sd, 'hot': hot, 'prof': prof, 'lowrom': low})
    tl = None
    if a.translate:
        tl = []
        for ln in open(a.translate):
            ln = ln.split('#')[0].strip()
            if ln: k = int(ln.split()[0], 16); tl.append((k & 0xFFFFFF, k >> 24))
    hdr, srcs = em.run(nfiles=a.files, translate=tl)
    # (I21) OBJECT RANGE: the tunable immediates for the interpreter (pc | kind << 24, 0-terminated);
    # checked against the ROM (16-bit SBC / ADC / CMP immediates)
    for pc, k in WS_TUNE.items():
        op = an.rom.b(pc)
        assert op in ((0xE9,), (0x69,), (0x69, 0xC9))[k], 'WS_TUNE: %06X is %02X' % (pc, op)
    srcs[0] += '\n#ifdef R65_SH2\n__attribute__((section(".lowtext.wstab")))\n#endif\nconst uint32_t r65_ws_tab[] = { %s, 0 };\n' % ', '.join(
        '0x%08X' % (pc | k << 24) for pc, k in sorted(WS_TUNE.items()))
    os.makedirs(a.out, exist_ok=True)
    open(os.path.join(a.out, 'game65.h'), 'w').write(hdr)
    for j, src in enumerate(srcs):
        open(os.path.join(a.out, 'game65_%02d.c' % j), 'w').write(src)
    ntr = sum(1 for i in an.insns.values() if i.traced)
    print('%d variants (%d traced), %d functions, %d translated, %d emitted insns, %d bad static paths' % (
        em.stats[0], ntr, em.stats[1], em.stats[2], em.stats[3], len(an.bad)))
    # function sizes for selection tools
    with open(os.path.join(a.out, 'owner.txt'), 'w') as f:
        for k, e in sorted(em.owner.items()):
            f.write('%08X %08X\n' % ((k[1] << 24) | k[0], (e[1] << 24) | e[0]))
    with open(os.path.join(a.out, 'funcs.txt'), 'w') as f:
        for e, b in sorted(em.funcs.items()):
            f.write('%08X %d %s\n' % ((e[1] << 24) | e[0], len(b), 'T' if e in em.tset else '-'))

if __name__ == '__main__':
    main()
