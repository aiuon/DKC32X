#!/usr/bin/env python3
"""ROM layout metadata for the build tool: addresses, sizes and file names only - no code, no data.

The developer runs `layout.py build` once on Yoshifanatic1's DKC1 disassembly; the build tool then
needs only tools/meta/layout.json.gz plus the user's ROM (every instruction byte and every data
byte comes from the ROM).  Contents:
  ins     {addr: operand_bytes}       65816 instruction boundaries of the disassembly (the operand
                                      size settles 8/16-bit immediates; the opcode comes from the ROM)
  labels  {name: addr}                CODE_/DATA_/ADDR_/UNK_xxxxxx labels
  named   {name: addr}                other labels (their position)
  refs    [addr]                      CODE_xxxxxx referenced as operands or in pointer tables
  inc     [[addr, path, gfxsize]]     incbin files and the label they follow (gfxsize: 0 if none)
  dlabs   [addr]                      DATA_ labels (order of the file)
  assets  [[category, name, start, end]]   the disassembly's asset pointer table
  spc     {"items": [...], "fmt": {opcode: [mnemonic, operand forms]}}  SPC700 engine layout + opcode formats
"""
import gzip, json, os, re, sys

D = os.path.dirname(os.path.abspath(__file__))
META = os.path.join(D, 'meta', 'layout.json.gz')
_cache = None


def load():
    global _cache
    if _cache is None:
        _cache = json.load(gzip.open(META, 'rt'))
    return _cache


def build(dis_dir, out=META):
    """(developer) parse the disassembly once"""
    sys.path.insert(0, os.path.join(D, 'recomp'))
    import disparse
    insns, labels, named, refs = disparse.parse(os.path.join(dis_dir, 'Routine_Macros_DKC1.asm'), raw=True)
    inc, dlabs = [], []
    lab = None
    for ln in open(os.path.join(dis_dir, 'Routine_Macros_DKC1.asm'), encoding='latin-1'):
        m = re.match(r'^(DATA|CODE)_([0-9A-F]{6}):', ln)
        if m:
            lab = int(m.group(2), 16) & 0x3FFFFF
            if m.group(1) == 'DATA':
                dlabs.append(lab)
            continue
        m = re.search(r'incbin "([^"]+)"(?:\s*;\s*GFXSize = \$([0-9A-F]+))?', ln)
        if m and lab is not None:
            inc.append([lab, m.group(1), int(m.group(2), 16) if m.group(2) else 0])
            lab = None
    assets, cat = [], None
    for line in open(os.path.join(dis_dir, 'AsarScripts/AssetPointersAndFiles.asm'), encoding='latin-1'):
        m = re.match(r"(\w+)PointersStart:", line.strip())
        if m:
            cat = m.group(1)
            continue
        m = re.match(r"\s*dl \$([0-9A-F]{6}),\$([0-9A-F]{6}),(\w+),", line)
        if m and cat:
            assets.append([cat, m.group(3), int(m.group(1), 16), int(m.group(2), 16)])
    sys.path.insert(0, os.path.join(D, 'audio'))
    import spc2x
    spc = spc2x.layout(os.path.join(dis_dir, 'SPC700', 'SPC700_Engine_DKC1.asm'))
    d = dict(ins={'%X' % a: v[1] for a, v in insns.items()}, labels=labels, named=named, refs=sorted(refs),
             inc=inc, dlabs=dlabs, assets=assets, spc=spc)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with gzip.open(out, 'wt') as f:
        json.dump(d, f, separators=(',', ':'))
    print('layout: %d instructions, %d labels, %d incbins, %d assets, %d SPC items -> %s (%d KB)' % (
        len(insns), len(labels), len(inc), len(assets), len(spc['items']), out, os.path.getsize(out) // 1024))


def insns(rom_bytes):
    """{addr24: (mnemonic, operand_bytes, 0, text)} like disparse.parse(); mnemonic from the ROM"""
    sys.path.insert(0, os.path.join(D, 'recomp'))
    from ops65 import OPS
    out = {}
    for k, n in load()['ins'].items():
        a = int(k, 16)
        mn = OPS[rom_bytes[a & 0x3FFFFF]][0]
        out[a] = (mn, n, 0, mn)
    return out


def labels(): return load()['labels']
def named(): return load()['named']
def refs(): return set(load()['refs'])
def incbins(): return [tuple(x) for x in load()['inc']]
def data_labels(): return load()['dlabs']
def assets(): return [tuple(x) for x in load()['assets']]
def spc(): return load()['spc']


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == 'build':
        build(sys.argv[2])
    else:
        print(__doc__)
