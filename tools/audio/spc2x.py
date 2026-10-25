#!/usr/bin/env python3
"""Static translator for the DKC1 SPC700 sound engine (Yoshifanatic1's disassembly,
DKC1/SPC700/SPC700_Engine_DKC1.asm) into basic blocks of a tiny IR, emitted as Python
(for validation against snes9x) or as C (for the 32X).

Only the instruction forms the engine actually uses are supported; anything else raises.
"""
import os
import re
import sys

HEX = r'\$([0-9A-Fa-f]+)'


def parse(path):
    """-> list of (label or None, mnemonic, operands[]) in source order, plus label addresses"""
    out = []
    for raw in open(path):
        line = raw.split(';')[0].rstrip()
        if not line.strip():
            continue
        s = line.strip()
        if s.startswith('base') or s.startswith('db') or s.startswith('dw'):
            out.append(('#data', s))
            continue
        m = re.match(r'^([A-Za-z_][A-Za-z0-9_]*):$', s)
        if m:
            out.append(('#label', m.group(1)))
            continue
        parts = s.split(None, 1)
        mn = parts[0].upper()
        ops = []
        if len(parts) > 1:
            ops = [o.strip() for o in re.split(r',(?![^(]*\))', parts[1])]
        out.append(('#ins', mn, ops))
    return out


def label_addr(name):
    m = re.match(r'(?:CODE|DATA)_([0-9A-Fa-f]{4})$', name)
    return int(m.group(1), 16) if m else None


def num(t):
    """'$1F' / 'DATA_11E1' / 'DATA_11E1+$01' -> int"""
    v = 0
    for part in t.split('+'):
        part = part.strip()
        if part.startswith('$'):
            v += int(part[1:], 16)
        elif part.startswith('DATA_') or part.startswith('CODE_'):
            v += label_addr(part)
        else:
            raise ValueError(t)
    return v


def operand(o):
    """-> (kind, value): A X Y YA SP imm dp dpx dpy abs absx absy indy indx"""
    u = o.strip()
    U = u.upper()
    if U in ('A', 'X', 'Y', 'YA', 'SP'):
        return (U, None)
    if u.startswith('#'):
        return ('imm', num(u[1:]))
    m = re.match(r'^\((\$[0-9A-Fa-f]{2})\)\+[yY]$', u)
    if m:
        return ('indy', num(m.group(1)))
    m = re.match(r'^\((.+)\+[xX]\)$', u)
    if m:
        return ('indx', num(m.group(1)))
    m = re.match(r'^(.+)\+([xXyY])$', u)
    if m:
        base, r = m.group(1), m.group(2).lower()
        v = num(base)
        two = re.fullmatch(r'\$[0-9A-Fa-f]{2}', base.strip()) is not None
        return (('dp' if two else 'abs') + r, v)
    if re.fullmatch(r'\$[0-9A-Fa-f]{2}', u):
        return ('dp', num(u))
    return ('abs', num(u))


ENGINE_ROM, ENGINE_LEN, ENGINE_ARAM = 0x0AA36A, 0x0D7E, 0x04E0   # DATA_8AA36A -> ARAM $04E0


def _isize(mn, ops):
    """instruction size from its operand forms (the engine's subset)"""
    n = 1
    for o in ops:
        u = o.strip().upper()
        if u in ('A', 'X', 'Y', 'YA', 'SP', '(X)', '(X)+', '(Y)', 'PSW', 'C'):
            continue
        if mn in ('BRA', 'BEQ', 'BNE', 'BPL', 'BMI', 'BCC', 'BCS'):
            n += 1
            continue
        k, v = operand(o)
        n += 2 if k in ('abs', 'absx', 'absy') or (mn == 'JMP' and k == 'indx') else 1
    return n


def _fmt(kind):
    return {'imm': '#$%02X', 'dp': '$%02X', 'dpx': '$%02X+X', 'dpy': '$%02X+Y', 'abs': '$%04X',
            'absx': '$%04X+X', 'absy': '$%04X+Y', 'indy': '($%02X)+Y', 'indx': '($%02X+X)', 'code': 'CODE_%04X',
            'jindx': '($%04X+X)'}[kind]


def layout(asm_path, rom_path=None):
    """(developer) the engine's layout + an opcode format table learnt from the listing against the
    ROM bytes: items [['L', name] | ['I', aram_addr] | ['D']], fmt {opcode: [mnemonic, [operand forms]]}
    (operand form: a register name, or [kind, byte source] with source 'b1', 'b2', 'w1', 'r1')"""
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
    rom = open(rom_path or os.environ.get('DKC_ROM', '/opt/work/dkc.sfc'), 'rb').read()
    eng = rom[ENGINE_ROM:ENGINE_ROM + ENGINE_LEN]
    items, fmt, pc = [], {}, None
    for it in parse(asm_path):
        if it[0] == '#data':
            m = re.match(r'base \$([0-9A-Fa-f]{4})', it[1])
            if m:
                pc = int(m.group(1), 16)
            elif pc is not None:
                d = it[1].split(None, 1)
                pc += (2 if d[0] == 'dw' else 1) * len(d[1].split(','))
            items.append(['D'])
            continue
        if it[0] == '#label':
            items.append(['L', it[1]])
            continue
        _, mn, ops = it
        sz = _isize(mn, ops)
        if pc is None or not (ENGINE_ARAM <= pc < ENGINE_ARAM + ENGINE_LEN):
            raise ValueError('instruction outside the engine block: %s %s' % (mn, ops))
        b = eng[pc - ENGINE_ARAM:pc - ENGINE_ARAM + sz]
        forms = []
        for o in ops:
            u = o.strip().upper()
            if u in ('A', 'X', 'Y', 'YA', 'SP', '(X)', '(X)+', '(Y)', 'PSW', 'C'):
                forms.append(u)
                continue
            if mn in ('BRA', 'BEQ', 'BNE', 'BPL', 'BMI', 'BCC', 'BCS') or o.strip().startswith('CODE_'):
                v, kind = label_addr(o.strip()) if o.strip().startswith('CODE_') else num(o), 'code'
            else:
                kind, v = operand(o)
                if mn == 'JMP' and kind == 'indx':
                    kind = 'jindx'          # JMP [!abs+X]
            cand = {'b1': b[1] if sz > 1 else None, 'b2': b[2] if sz > 2 else None,
                    'w1': (b[1] | b[2] << 8) if sz > 2 else None,
                    'r1': ((pc + sz + (b[sz - 1] - 256 if b[sz - 1] & 0x80 else b[sz - 1])) & 0xFFFF) if sz > 1 else None}
            order = ('r1',) if mn in ('BRA', 'BEQ', 'BNE', 'BPL', 'BMI', 'BCC', 'BCS') else \
                    ('w1',) if kind in ('abs', 'absx', 'absy', 'code', 'jindx') else ('b1', 'b2')
            src = [k for k in order if cand[k] == v]
            if not src:
                raise ValueError('no byte source for %s %s at %04X (%s)' % (mn, ops, pc, b.hex()))
            forms.append([kind, src[0]])
        key = '%02X' % b[0]
        f = [mn, forms]
        if key in fmt and fmt[key] != f:
            # (two operands of the same width: the second occurrence may pick the other byte)
            if not (fmt[key][0] == mn and len(fmt[key][1]) == len(forms)):
                raise ValueError('opcode %s: %s vs %s' % (key, fmt[key], f))
        fmt.setdefault(key, f)
        items.append(['I', pc])
        pc += sz
    return dict(items=items, fmt=fmt)


def from_rom(rom, lay):
    """the parse() item list rebuilt from the user's ROM bytes and the shipped layout"""
    eng = rom[ENGINE_ROM:ENGINE_ROM + ENGINE_LEN]
    out = []
    for it in lay['items']:
        if it[0] == 'D':
            out.append(('#data', ''))
        elif it[0] == 'L':
            out.append(('#label', it[1]))
        else:
            pc = it[1]
            b = eng[pc - ENGINE_ARAM:]
            mn, forms = lay['fmt']['%02X' % b[0]]
            ops = []
            sz = 1 + max([2 if f[1] == 'w1' else 2 if f[1] == 'b2' else 1 for f in forms if isinstance(f, list)] + [0])
            for f in forms:
                if not isinstance(f, list):
                    ops.append(f)
                    continue
                kind, src = f
                if src == 'b1': v = b[1]
                elif src == 'b2': v = b[2]
                elif src == 'w1': v = b[1] | b[2] << 8
                else:
                    r = b[sz - 1]
                    v = (pc + sz + (r - 256 if r & 0x80 else r)) & 0xFFFF
                ops.append(_fmt(kind) % v)
            out.append(('#ins', mn, ops))
    return out


class Block:
    def __init__(self, name):
        self.name = name
        self.ins = []       # (mn, ops)
        self.next = None    # fallthrough block name


def blocks_of(src):
    """split into basic blocks: a new block at every label and after every control transfer"""
    blocks, order = {}, []
    cur = None
    n = [0]

    def new(name):
        nonlocal cur
        b = Block(name)
        if cur is not None and cur.next is None and not cur.terminated:
            cur.next = name
        cur = b
        cur.terminated = False
        blocks[name] = b
        order.append(name)

    for item in src:
        if item[0] == '#label':
            if item[1].startswith('CODE_'):
                new(item[1])
            else:
                if cur is not None and not cur.terminated:
                    cur.terminated = True     # data follows code: no fallthrough
                cur = None
            continue
        if item[0] == '#data':
            continue
        _, mn, ops = item
        if cur is None:
            continue
        cur.ins.append((mn, ops))
        if mn in ('BRA', 'JMP', 'RET'):
            cur.terminated = True
            n[0] += 1
            new('L%d' % n[0])
        elif mn in ('BEQ', 'BNE', 'BPL', 'BMI', 'BCC', 'BCS', 'CALL'):
            n[0] += 1
            new('L%d' % n[0])
    return blocks, order


# ------------------------------------------------------------------ Python emitter

def py_read(k, v):
    if k == 'imm':
        return '0x%X' % v
    if k in ('A', 'X', 'Y'):
        return 's.' + k.lower()
    if k == 'dp':
        return 's.rd(0x%X)' % v
    if k == 'dpx':
        return 's.rd((0x%X + s.x) & 0xFF)' % v
    if k == 'abs':
        return 's.rd(0x%X)' % v
    if k == 'absx':
        return 's.rd((0x%X + s.x) & 0xFFFF)' % v
    if k == 'absy':
        return 's.rd((0x%X + s.y) & 0xFFFF)' % v
    if k == 'indy':
        return 's.rd((s.rw(0x%X) + s.y) & 0xFFFF)' % v
    raise ValueError(k)


def py_addr(k, v):
    if k == 'dp' or k == 'abs':
        return '0x%X' % v
    if k == 'dpx':
        return '(0x%X + s.x) & 0xFF' % v
    if k == 'absx':
        return '(0x%X + s.x) & 0xFFFF' % v
    if k == 'absy':
        return '(0x%X + s.y) & 0xFFFF' % v
    if k == 'indy':
        return '(s.rw(0x%X) + s.y) & 0xFFFF' % v
    raise ValueError(k)


def emit_py(blocks, order):
    L = ['# generated by tools/audio/spc2x.py', 'def build(s):', '    B = {}']
    for name in order:
        b = blocks[name]
        L.append('    def b_%s():' % name)
        body = []
        term = None
        for mn, ops in b.ins:
            o = [operand(x) for x in ops]
            if mn == 'MOV':
                (dk, dv), (sk, sv) = o
                if dk in ('A', 'X', 'Y'):
                    if sk == 'SP':
                        raise ValueError('mov from sp')
                    body.append('s.%s = %s; s.nz(s.%s)' % (dk.lower(), py_read(sk, sv), dk.lower()))
                elif dk == 'SP':
                    body.append('pass  # MOV SP, X')
                else:
                    body.append('s.wr(%s, %s)' % (py_addr(dk, dv), py_read(sk, sv)))
            elif mn == 'MOVW':
                (dk, dv), (sk, sv) = o
                if dk == 'YA':
                    body.append('w = s.rw(0x%X); s.a = w & 0xFF; s.y = w >> 8; s.nz16(w)' % sv)
                else:
                    body.append('s.wr(0x%X, s.a); s.wr(0x%X, s.y)' % (dv, (dv + 1) & 0xFF))
            elif mn in ('ADC', 'SBC', 'AND', 'OR', 'EOR', 'CMP'):
                (dk, dv), (sk, sv) = o
                src = py_read(sk, sv)
                if dk in ('A', 'X', 'Y'):
                    dst_r, dst_w = 's.' + dk.lower(), 's.%s = {}' % dk.lower()
                else:
                    dst_r, dst_w = py_read(dk, dv), 's.wr(%s, {})' % py_addr(dk, dv)
                op = {'ADC': 's.adc({d}, {s})', 'SBC': 's.sbc({d}, {s})', 'AND': 's.nz({d} & {s})',
                      'OR': 's.nz({d} | {s})', 'EOR': 's.nz({d} ^ {s})', 'CMP': 's.cmp({d}, {s})'}[mn]
                expr = op.format(d=dst_r, s=src)
                if mn == 'CMP':
                    body.append(expr)
                else:
                    body.append(dst_w.format(expr))
            elif mn in ('INC', 'DEC', 'ASL', 'LSR', 'ROR', 'XCN'):
                (dk, dv), = o
                f = {'INC': 's.nz(({}) + 1 & 0xFF)', 'DEC': 's.nz(({}) - 1 & 0xFF)', 'ASL': 's.asl({})',
                     'LSR': 's.lsr({})', 'ROR': 's.ror({})', 'XCN': 's.nz((({0}) >> 4 | ({0}) << 4) & 0xFF)'}[mn]
                if dk in ('A', 'X', 'Y'):
                    body.append('s.%s = %s' % (dk.lower(), f.format('s.' + dk.lower())))
                else:
                    body.append('t = %s; s.wr(%s, %s)' % (py_read(dk, dv), py_addr(dk, dv), f.format('t')))
            elif mn == 'MUL':
                body.append('w = s.y * s.a; s.a = w & 0xFF; s.y = w >> 8; s.nz(s.y)')
            elif mn in ('ADDW', 'SUBW', 'CMPW'):
                (dk, dv), (sk, sv) = o
                body.append('s.%s(0x%X)' % (mn.lower(), sv))
            elif mn == 'PUSH':
                body.append('s.stack.append(s.%s)' % ops[0].lower())
            elif mn == 'POP':
                body.append('s.%s = s.stack.pop()' % ops[0].lower())
            elif mn == 'CLRC':
                body.append('s.c = 0')
            elif mn == 'SETC':
                body.append('s.c = 1')
            elif mn == 'CLRP':
                body.append('pass')
            elif mn in ('BEQ', 'BNE', 'BPL', 'BMI', 'BCC', 'BCS'):
                cond = {'BEQ': 's.z', 'BNE': 'not s.z', 'BPL': 'not s.n', 'BMI': 's.n',
                        'BCC': 'not s.c', 'BCS': 's.c'}[mn]
                term = 'return %r if %s else %r' % (ops[0], cond, b.next)
            elif mn in ('BRA', 'JMP'):
                t = ops[0]
                m = re.match(r'^\((.+)\+[xX]\)$', t)
                if m:
                    term = "return 'CODE_%%04X' %% s.rw((0x%X + s.x) & 0xFFFF)" % num(m.group(1))
                else:
                    term = 'return %r' % t
            elif mn == 'CALL':
                term = 's.stack.append(%r); return %r' % (b.next, ops[0])
            elif mn == 'RET':
                term = 'return s.stack.pop()'
            else:
                raise ValueError('unsupported %s %s' % (mn, ops))
        if term is None:
            term = 'return %r' % b.next
        for x in body:
            L.append('        ' + x)
        L.append('        ' + term)
        L.append("    B['%s'] = b_%s" % (name, name))
    L.append('    return B')
    return '\n'.join(L) + '\n'


# ------------------------------------------------------------------ C emitter
RAMTOP = 0x600      # engine variables + instrument table live below this; above = read-only song image


def c_read(k, v):
    if k == 'imm':
        return '0x%X' % v
    if k in ('A', 'X', 'Y'):
        return k.lower()
    if k == 'dp':
        return c_rd_const(v)
    if k == 'abs':
        return c_rd_const(v)
    if k == 'dpx':
        return 'RDX((0x%X + x) & 0xFF)' % v
    if k == 'absx':
        return 'RDX((0x%X + x) & 0xFFFF)' % v
    if k == 'absy':
        return 'RDX((0x%X + y) & 0xFFFF)' % v
    if k == 'indy':
        return 'RDX((RW(0x%X) + y) & 0xFFFF)' % v
    raise ValueError(k)


def c_rd_const(v):
    if v == 0xF3:
        return 's->dsp[s->ram[0xF2] & 0x7F]'
    if 0xF4 <= v <= 0xF7:
        return 's->port[%d]' % (v - 0xF4)
    if v in (0xFD, 0xFE, 0xFF):
        return '1'
    if v < RAMTOP:
        return 's->ram[0x%X]' % v
    return 's->rom[0x%X]' % v


def c_write(k, v, val):
    if k in ('dp', 'abs'):
        if v == 0xF3:
            return 'DSPW(%s);' % val
        if v < RAMTOP:
            return 's->ram[0x%X] = (%s);' % (v, val)
        return '(void)(%s);' % val
    addr = {'dpx': '(0x%X + x) & 0xFF', 'absx': '(0x%X + x) & 0xFFFF', 'absy': '(0x%X + y) & 0xFFFF',
            'indy': '(RW(0x%X) + y) & 0xFFFF'}[k] % v
    return 'WRX(%s, %s);' % (addr, val)


def emit_c(blocks, order, entries):
    ids = {name: i for i, name in enumerate(order)}
    codes = sorted(n for n in order if n.startswith('CODE_'))
    L = ['/* generated by tools/audio/spc2x.py from the DKC1 SPC700 engine disassembly - do not edit */',
         '#include "spceng.h"',
         '#define RDX(a) spc_rdx(s, (a))',
         '#define WRX(a, v) spc_wrx(s, (a), (v))',
         '#define RW(d) (s->ram[(d)] | s->ram[((d) + 1) & 0xFF] << 8)',
         '#define DSPW(v) spc_dspw(s, (v))',
         '#define NZ(v) (nzv = (v) & 0xFF, fn = nzv & 0x80, fz = !nzv)',
         '#define NZ16(v) (fn = (v) & 0x8000, fz = !((v) & 0xFFFF))',
         'void spc_exec(spc_t *s, int entry)',
         '{',
         '    unsigned a = s->a, x = s->x, y = s->y, t, w, nzv;',
         '    int fn = 0, fz = 0, c = 0;',
         '    uint16_t rs[16]; int rsp = 0;',
         '    uint8_t ds[16]; int dsp = 0;',
         '    switch (entry) {']
    for e in entries:
        L.append('    case SPC_%s: rs[rsp++] = 0xFFFF; goto %s;' % (e, e))
    L.append('    default: return;')
    L.append('    }')
    for name in order:
        b = blocks[name]
        L.append('%s:' % name)
        if name == 'CODE_05FE':
            L.append('    goto out;   /* the port-poll loop: the 32X driver calls the tick directly */')
        for mn, ops in b.ins:
            o = [operand(xx) for xx in ops]
            if mn == 'MOV':
                (dk, dv), (sk, sv) = o
                if dk in ('A', 'X', 'Y'):
                    L.append('    %s = %s; NZ(%s);' % (dk.lower(), c_read(sk, sv), dk.lower()))
                elif dk == 'SP':
                    L.append('    /* MOV SP, X */')
                else:
                    L.append('    ' + c_write(dk, dv, c_read(sk, sv)))
            elif mn == 'MOVW':
                (dk, dv), (sk, sv) = o
                if dk == 'YA':
                    L.append('    w = RW(0x%X); a = w & 0xFF; y = w >> 8; NZ16(w);' % sv)
                else:
                    L.append('    s->ram[0x%X] = a; s->ram[0x%X] = y;' % (dv, (dv + 1) & 0xFF))
            elif mn in ('ADC', 'SBC', 'AND', 'OR', 'EOR', 'CMP'):
                (dk, dv), (sk, sv) = o
                src = c_read(sk, sv)
                dst = dk.lower() if dk in ('A', 'X', 'Y') else c_read(dk, dv)
                L.append('    t = %s;' % src)
                if mn == 'ADC':
                    L.append('    w = (%s) + t + c; c = w > 0xFF; NZ(w);' % dst)
                elif mn == 'SBC':
                    L.append('    w = (%s) - t - !c; c = (int)w >= 0; NZ(w);' % dst)
                elif mn == 'CMP':
                    L.append('    w = (%s) - t; c = (int)w >= 0; NZ(w);' % dst)
                else:
                    opc = {'AND': '&', 'OR': '|', 'EOR': '^'}[mn]
                    L.append('    w = (%s) %s t; NZ(w);' % (dst, opc))
                if mn != 'CMP':
                    if dk in ('A', 'X', 'Y'):
                        L.append('    %s = w & 0xFF;' % dk.lower())
                    else:
                        L.append('    ' + c_write(dk, dv, 'w & 0xFF'))
            elif mn in ('INC', 'DEC', 'ASL', 'LSR', 'ROR', 'XCN'):
                (dk, dv), = o
                src = dk.lower() if dk in ('A', 'X', 'Y') else c_read(dk, dv)
                L.append('    t = %s;' % src)
                L.append('    ' + {'INC': 'w = t + 1;', 'DEC': 'w = t - 1;', 'ASL': 'c = t >> 7; w = t << 1;',
                                   'LSR': 'c = t & 1; w = t >> 1;', 'ROR': 'w = t >> 1 | c << 7; c = t & 1;',
                                   'XCN': 'w = t >> 4 | t << 4;'}[mn] + ' NZ(w);')
                if dk in ('A', 'X', 'Y'):
                    L.append('    %s = w & 0xFF;' % dk.lower())
                else:
                    L.append('    ' + c_write(dk, dv, 'w & 0xFF'))
            elif mn == 'MUL':
                L.append('    w = y * a; a = w & 0xFF; y = w >> 8; NZ(y);')
            elif mn in ('ADDW', 'SUBW', 'CMPW'):
                (dk, dv), (sk, sv) = o
                op = '+' if mn == 'ADDW' else '-'
                L.append('    w = (y << 8 | a) %s RW(0x%X);' % (op, sv))
                L.append('    c = %s;' % ('w > 0xFFFF' if mn == 'ADDW' else '(int)w >= 0'))
                L.append('    NZ16(w);')
                if mn != 'CMPW':
                    L.append('    a = w & 0xFF; y = (w >> 8) & 0xFF;')
            elif mn == 'PUSH':
                L.append('    ds[dsp++] = %s;' % ops[0].lower())
            elif mn == 'POP':
                L.append('    %s = ds[--dsp];' % ops[0].lower())
            elif mn == 'CLRC':
                L.append('    c = 0;')
            elif mn == 'SETC':
                L.append('    c = 1;')
            elif mn == 'CLRP':
                pass
            elif mn in ('BEQ', 'BNE', 'BPL', 'BMI', 'BCC', 'BCS'):
                cond = {'BEQ': 'fz', 'BNE': '!fz', 'BPL': '!fn', 'BMI': 'fn', 'BCC': '!c', 'BCS': 'c'}[mn]
                L.append('    if (%s) goto %s;' % (cond, ops[0]))
            elif mn in ('BRA', 'JMP'):
                t = ops[0]
                m = re.match(r'^\((.+)\+[xX]\)$', t)
                if m:
                    L.append('    switch (spc_rdx(s, (0x%X + x) & 0xFFFF) | spc_rdx(s, (0x%X + x) & 0xFFFF) << 8) {'
                             % (num(m.group(1)), num(m.group(1)) + 1))
                    for cn in codes:
                        L.append('    case 0x%04X: goto %s;' % (label_addr(cn), cn))
                    L.append('    default: goto out;')
                    L.append('    }')
                elif t == 'CODE_05FE':
                    L.append('    goto out;')
                else:
                    L.append('    goto %s;' % t)
            elif mn == 'CALL':
                L.append('    rs[rsp++] = %d; goto %s;' % (ids[b.next], ops[0]))
            elif mn == 'RET':
                L.append('    goto ret;')
            else:
                raise ValueError('unsupported %s %s' % (mn, ops))
        if b.next and not (b.ins and b.ins[-1][0] in ('BRA', 'JMP', 'RET', 'CALL')):
            if b.next == 'CODE_05FE':
                L.append('    goto out;')
            else:
                L.append('    goto %s;' % b.next)
        elif not b.ins and b.next:
            L.append('    goto %s;' % b.next)
    L.append('ret:')
    L.append('    switch (rs[--rsp]) {')
    for name in order:
        L.append('    case %d: goto %s;' % (ids[name], name))
    L.append('    default: goto out;')
    L.append('    }')
    L.append('out:')
    L.append('    s->a = a; s->x = x; s->y = y;')
    L.append('    (void)fn; (void)fz; (void)c; (void)t; (void)nzv;')
    L.append('}')
    return '\n'.join(L) + '\n'


if __name__ == '__main__':
    if sys.argv[1].endswith('.asm'):
        src = parse(sys.argv[1])
    else:   # the user's ROM + the shipped layout (tools/meta/layout.json.gz)
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
        import layout as _lay
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
        from dkcrom import Rom
        src = from_rom(Rom(sys.argv[1]).data, _lay.spc())
    blocks, order = blocks_of(src)
    if sys.argv[2] == 'py':
        open(sys.argv[3], 'w').write(emit_py(blocks, order))
    elif sys.argv[2] == 'c':
        open(sys.argv[3], 'w').write(emit_c(blocks, order, ['CODE_1071', 'CODE_064F', 'CODE_1173']))
    print(len(order), 'blocks')
