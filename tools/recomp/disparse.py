"""Parse the DKC1 disassembly (Routine_Macros_DKC1.asm) into instruction boundaries.
Returns {addr24: (mnemonic, operand_bytes, line_no, text)} and {label: addr24} for hex labels,
plus code references (CODE_xxxxxx used as operands / in dw/dl tables)."""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..'))
import paths
import re, os, sys
DIS = os.path.join(paths.DISASM, 'Routine_Macros_DKC1.asm')
LBL = re.compile(r'^([A-Za-z_][A-Za-z0-9_]*):')
HEXL = re.compile(r'^(?:CODE|DATA|ADDR|UNK)_([0-9A-F]{6})$')
INS = re.compile(r'^\s+([A-Z]{3})(?:\.([bwl]))?(?:\s+(.*?))?\s*(?:;.*)?$')
SZ = {'b': 1, 'w': 2, 'l': 3}

def parse(path=None, raw=False, rom=None):
    """raw / path: parse the disassembly itself (tools/layout.py build); otherwise the shipped layout
    metadata with the mnemonics read from rom (the user's unpatched ROM bytes)"""
    if not raw and path is None:
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
        import layout
        if rom is None:
            rom = open(os.environ['DKC_ROM'], 'rb').read()
            if len(rom) % 0x8000 == 0x200: rom = rom[0x200:]
        return layout.insns(rom), dict(layout.labels()), dict(layout.named()), layout.refs()
    path = path or DIS
    insns, labels, named, refs = {}, {}, {}, set()
    cur = None
    for no, line in enumerate(open(path, encoding='latin-1'), 1):
        m = LBL.match(line)
        if m:
            name = m.group(1); h = HEXL.match(name)
            if h: cur = int(h.group(1), 16); labels[name] = cur
            elif cur is not None: named[name] = cur
            line = line[m.end():]
            if not line.strip(): continue
            line = '\t' + line.strip()
        for r in re.findall(r'CODE_([0-9A-F]{6})', line.split(';')[0]):
            refs.add(int(r, 16))
        if cur is None: continue
        m = INS.match(line)
        if not m:
            s = line.strip()
            if not s or s.startswith(';'): continue
            # data / directives: lose sync until next hex label
            if re.match(r'^(db|dw|dl|dd|incbin|%|if|else|endif|endmacro|macro|!|org|base|warn|assert|print|skip|fillbyte|padbyte|pad|fill|namespace|table|cleartable|arch|function|incsrc|check|optimize|struct|endstruct|bank|warnpc|norom|lorom|hirom)', s, re.I):
                cur = None
            else:
                cur = None
            continue
        mn, sfx, opd = m.group(1), m.group(2), (m.group(3) or '')
        if mn in ('MVN', 'MVP'): n = 2
        elif sfx: n = SZ[sfx]
        else: n = 0
        insns[cur] = (mn, n, no, line.strip())
        cur += 1 + n
        if (cur & 0xFFFF) == 0: cur = None
    return insns, labels, named, refs

if __name__ == '__main__':
    import sys
    sys.path.insert(0, os.path.dirname(__file__))
    from ops65 import OPS
    insns, labels, named, refs = parse()
    rom = open('/opt/work/dkc.sfc', 'rb').read()
    bad = 0
    for a, (mn, n, no, t) in insns.items():
        op = rom[a & 0x3FFFFF]
        if OPS[op][0] != mn:
            bad += 1
            if bad < 20: print('mismatch %06x %s rom %02x %s line %d' % (a, t, op, OPS[op][0], no))
    print(len(insns), 'insns', bad, 'bad', len(labels), 'labels', len(named), 'named', len(refs), 'refs')
