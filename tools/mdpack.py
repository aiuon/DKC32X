#!/usr/bin/env python3
"""mdpack.py md.elf md.bin: (I22) LZSS-pack the 68000's RAM image (.data + RAM-resident code) at the end of
md.bin (md/boot.s unpacks it into work RAM at power-on / reset) - ~4.4 KB more room in the ROM's first 256 KB."""
import os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lzss
elf, out = sys.argv[1], sys.argv[2]
nmtool = os.environ.get('M68K_NM', 'm68k-linux-gnu-nm')
nm = subprocess.run([nmtool, elf], capture_output=True, text=True, check=True).stdout
sym = {l.split()[2]: int(l.split()[0], 16) for l in nm.splitlines() if len(l.split()) == 3}
te, ds = sym['__text_end'], sym['__data_size']
b = open(out, 'rb').read()
o = te - 0x8803F0
assert o + ds == len(b), 'md.bin: the RAM image is not at its end (%d + %d != %d)' % (o, ds, len(b))
z = lzss.compress_opt(b[o:])          # (I24: optimal parse, the same format)
assert lzss.decompress(z, ds) == b[o:]
open(out, 'wb').write(b[:o] + z + (b'\0' if len(z) & 1 else b''))
print('mdpack: 68000 RAM image %d -> %d bytes' % (ds, len(z)))
