#!/usr/bin/env python3
"""Assemble the 32X ROM: code at 0, the resident pack at 0x040000, the game code + data at GAME_BASE
(both must end below 0x380000: cart window 7 is banked), then the level banks (SSF2 mapper, 512 KB
each): bank k = physical ROM bank 7 + k (bank 0 sits at 0x380000 inside the first 4 MB).
  finalize.py sh.bin pack.bin out.32x [gtext.bin GAME_BASE [banks.bin [worldbanks.bin [sdgame.bin LMA
              [music.bin FM_DIR fmdrv.bin]]]]]
(I24) SOUND FM: the music block after the game code (the end of the resident ROM, its SNES-only samples
last: in cart window 5), and the FM pages at the end - copies of bank 5 whose tail has the FM directory, the
Z80 driver and songs instead of those samples (md/fm.c maps one into window 5 while SOUND FM plays)."""
import json, os, struct, sys
code = bytearray(open(sys.argv[1], 'rb').read())
pack = open(sys.argv[2], 'rb').read()
assert len(code) <= 0x40000, "SH-2/68K code over 256KB"
d = code + b'\xff' * (0x40000 - len(code)) + pack
if len(sys.argv) > 5:
    gt = open(sys.argv[4], 'rb').read(); base = int(sys.argv[5], 16) - 0x02000000
    assert len(d) <= base, "pack overlaps the game code"
    d += b'\xff' * (base - len(d)) + gt
    print("game code+data %d KB at 0x%X, resident end 0x%X (limit 0x380000)" % (len(gt) // 1024, base, len(d)))
fmj = None
if len(sys.argv) > 12:
    music = open(sys.argv[10], 'rb').read()
    fmdir = sys.argv[11]
    fmj = json.load(open(os.path.join(fmdir, 'fm.json')))
    d += b'\xff' * (-len(d) % 16)
    mus_at = len(d)
    d += music
    print("music %d KB at 0x%X, its SNES-only samples from 0x%X (%d KB)" % (
        len(music) // 1024, mus_at, mus_at + fmj['swap_off'], (len(music) - fmj['swap_off']) // 1024))
    hp = 0x40000
    assert d[hp:hp + 4] == b'DKCP' and d[hp + 24:hp + 28] == b'\0' * 4
    d[hp + 24:hp + 28] = struct.pack('>I', mus_at - hp)
    assert len(d) <= 0x300000, "resident ROM + music over 3 MB: 0x%X" % len(d)
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
if fmj is not None:
    # the FM pages.  Tail: 'FMD1', u16 nsongs, u16 0, u32 driver offset, u16 driver length, u16 0, then per
    # song u32 (FM page bank << 24 | its block's window-5 offset; 0 = none), the driver, the songs (best fit)
    B5, B6 = 0x280000, 0x300000
    tail = max(B5, mus_at + fmj['swap_off'])
    tail += -tail % 16
    drv = open(sys.argv[12], 'rb').read()
    names = fmj['names']
    ns = len(names)
    dir_n = 16 + 4 * ns
    o_drv = tail - B5 + dir_n
    o_song = o_drv + len(drv) + (-len(drv) % 16)
    room = B6 - B5 - o_song
    blocks = {}
    for k, e in fmj['songs'].items():
        blocks[int(k)] = open(os.path.join(fmdir, fmj['blocks'][k]), 'rb').read()
    # (best-fit decreasing; a song that fits no page: no FM for it - the 32X plays it all)
    pages = []                                   # [free, [(song, window-5 offset)]]
    maxp = int(os.environ.get('FM_MAXPAGES', '6'))
    for sg in sorted(blocks, key=lambda k: -len(blocks[k])):
        n = len(blocks[sg]) + (len(blocks[sg]) & 1)
        fit = [pg for pg in pages if pg[0] >= n]
        if not fit and n <= room and len(pages) < maxp:
            pages.append([room, []]); fit = [pages[-1]]
        if not fit: continue
        pg = min(fit, key=lambda pg: pg[0])
        pg[1].append((sg, o_song + room - pg[0]))
        pg[0] -= n
    d += b'\xff' * max(0, B6 - len(d))
    placed = set(sg for pg in pages for sg, _ in pg[1])
    off_songs = struct.unpack_from('>I', music, 8)[0]
    for sg in range(ns):                         # (left out: the FM flag cleared in its song record)
        if sg not in placed:
            if sg in blocks: print('FM: no room for song %d (%s): the 32X plays it' % (sg, names[sg]))
            d[mus_at + off_songs + 96 * sg + 8 + 80 + 5] &= 0xFE
    size0 = (len(d) + 0x7FFFF) & ~0x7FFFF        # (the pages after everything: whole banks)
    d += b'\xff' * (size0 - len(d))
    first = size0 // 0x80000
    ent = [0] * ns
    for pi, (free, lst) in enumerate(pages):
        for sg, o in lst: ent[sg] = (first + pi) << 24 | o
    hdr = b'FMD1' + struct.pack('>HHIHH', ns, 0, o_drv, len(drv), 0) + b''.join(struct.pack('>I', x) for x in ent)
    assert len(hdr) == dir_n
    hp = 0x40000
    d[hp + 52:hp + 56] = struct.pack('>I', first << 24 | (tail - B5))
    base5 = bytes(d[B5:B6])
    for pi, (free, lst) in enumerate(pages):
        pg = bytearray(base5)
        pg[tail - B5:] = b'\0' * (B6 - tail)
        pg[tail - B5:tail - B5 + dir_n] = hdr
        pg[o_drv:o_drv + len(drv)] = drv
        for sg, o in lst: pg[o:o + len(blocks[sg])] = blocks[sg]
        d += pg
    print("FM pages: %d (banks %d-%d), tail %d KB each from 0x%X: %d of %d songs, %d KB of song data" % (
        len(pages), first, first + len(pages) - 1, (B6 - tail) // 1024, tail, len(placed), len(blocks),
        sum(len(blocks[sg]) for sg in placed) // 1024))
size = (len(d) + 0x7FFFF) & ~0x7FFFF      # (whole 512 KB banks, no power-of-2 padding: <= 15 MB for
                                           #  the Mega EverDrive Pro)
assert size <= 0x400000 or banks, "ROM over 4MB"
d += b'\xff' * (size - len(d))
d[0x1A4:0x1A8] = (size - 1).to_bytes(4, 'big')
if size > 0x400000:
    # (I21q) over 4 MB: the system field says "SEGA SSF" - flash carts (Mega EverDrive X / PRO, MegaSD) then
    # use the Sega (SSF) bank mapper; with "SEGA 32X" the EverDrive PRO picked its linear "10M" mapping,
    # which "conflicts with 32X addon" (the 32X owns $800000-$9FFFFF).  Doom 32X Fusion (d32xr) does the
    # same.  Emulators / MiSTer choose the mapper by size (PicoDrive, MiSTer) or by the 32X loader (ares).
    d[0x100:0x110] = b'SEGA SSF        '
cs = 0
for i in range(0x200, len(d), 2):
    cs = (cs + (d[i] << 8 | d[i + 1])) & 0xFFFF
d[0x18E:0x190] = cs.to_bytes(2, 'big')
open(sys.argv[3], 'wb').write(d)
print("wrote %s (%d KB, %d level banks, %d world banks)" % (sys.argv[3], len(d) // 1024, len(banks) // 0x80000, len(wbanks) // 0x80000))
