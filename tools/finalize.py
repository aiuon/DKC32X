#!/usr/bin/env python3
"""Assemble the 32X ROM: code at 0, the resident pack at 0x040000, the game code + data at GAME_BASE
(both must end below 0x380000: cart window 7 is banked), then the level banks (SSF2 mapper, 512 KB
each): bank k = physical ROM bank 7 + k (bank 0 sits at 0x380000 inside the first 4 MB).
  finalize.py sh.bin pack.bin out.32x [gtext.bin GAME_BASE [banks.bin [worldbanks.bin [sdgame.bin LMA]]]]"""
import sys
code = bytearray(open(sys.argv[1], 'rb').read())
pack = open(sys.argv[2], 'rb').read()
assert len(code) <= 0x40000, "SH-2/68K code over 256KB"
d = code + b'\xff' * (0x40000 - len(code)) + pack
if len(sys.argv) > 5:
    gt = open(sys.argv[4], 'rb').read(); base = int(sys.argv[5], 16) - 0x02000000
    assert len(d) <= base, "pack overlaps the game code"
    d += b'\xff' * (base - len(d)) + gt
    print("game code+data %d KB at 0x%X, resident end 0x%X (limit 0x380000)" % (len(gt) // 1024, base, len(d)))
banks = open(sys.argv[6], 'rb').read() if len(sys.argv) > 6 else b''
wbanks = open(sys.argv[7], 'rb').read() if len(sys.argv) > 7 else b''
if wbanks:
    # all-worlds ROM: window 6 (0x300000) = world bank 0 (physical bank 6), the other world banks
    # after the level banks (build_viewer.py numbers them 7 + nlevelbanks + i - 1)
    assert len(d) <= 0x300000, "resident ROM over 3 MB (windows 6 / 7 are banked): 0x%X" % len(d)
    d += b'\xff' * (0x300000 - len(d))
    d += wbanks[:0x80000]
if banks:
    assert len(d) <= 0x380000, "resident ROM over 3.5 MB (window 7 is the banked level data)"
    d += b'\xff' * (0x380000 - len(d))
    d += banks
if wbanks:
    d += wbanks[0x80000:]
if len(sys.argv) > 9:
    # (I17) the SDRAM game code image in world bank 0 at its LMA (window 6 at power-on), after a 16-byte
    # header m_main checks: 'SDGM', length, sum of the image's 32-bit words, ~sum
    img = open(sys.argv[8], 'rb').read(); lma = int(sys.argv[9], 16)
    assert 0x300010 <= lma and lma + len(img) <= 0x380000 and len(img) % 4 == 0, 'sdgame image 0x%X + %d' % (lma, len(img))
    assert len(d) >= lma + len(img), 'world bank 0 missing'
    assert all(b == 0xFF for b in d[lma - 16:lma + len(img)]), 'world bank 0 too full for the SDRAM code image'
    sm = sum(int.from_bytes(img[i:i + 4], 'big') for i in range(0, len(img), 4)) & 0xFFFFFFFF
    d[lma - 16:lma] = b'SDGM' + len(img).to_bytes(4, 'big') + sm.to_bytes(4, 'big') + (sm ^ 0xFFFFFFFF).to_bytes(4, 'big')
    d[lma:lma + len(img)] = img
    print('SDRAM code image %d KB in world bank 0 at 0x%X' % (len(img) // 1024, lma))
size = (len(d) + 0x7FFFF) & ~0x7FFFF      # (whole 512 KB banks, no power-of-2 padding: <= 15 MB for
                                           #  the Mega EverDrive Pro)
assert size <= 0x400000 or banks, "ROM over 4MB"
d += b'\xff' * (size - len(d))
d[0x1A4:0x1A8] = (size - 1).to_bytes(4, 'big')
cs = 0
for i in range(0x200, len(d), 2):
    cs = (cs + (d[i] << 8 | d[i + 1])) & 0xFFFF
d[0x18E:0x190] = cs.to_bytes(2, 'big')
open(sys.argv[3], 'wb').write(d)
print("wrote %s (%d KB, %d level banks, %d world banks)" % (sys.argv[3], len(d) // 1024, len(banks) // 0x80000, len(wbanks) // 0x80000))
