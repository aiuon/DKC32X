#!/usr/bin/env python3
"""All-worlds ROM: build the cart window-6 world banks (after the SH-2 link).

Each 512 KB bank = the game's SNES page table snes_pg[4096] (u32 big-endian SH-2 addresses, at
0x02300000: build.sh links snes_pg there with -DSNES_PG_BANKED) + that world's SNES ROM pages.
Resident pages point into snes_blob (the common pages, .gtext), the world's own pages into the
bank (0x02304000 + j * 1024), pages nobody needs to zero_page.

  worldbanks.py ROM build/game/worlds.json build/sh.elf OUT.bin
(personal-use data: the pages are the user's own ROM, read at build time)"""
import json, os, subprocess, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'recomp'))
import rompatch

BANK = 0x80000
BASE = 0x02300000
TABLE = 4096 * 4
BANK0_END = 0x6F000 - 16                 # (tools/build.py SDGAME_LMA - 0x300000, its header)


def syms(elf):
    out = subprocess.run([os.environ.get('DKC_NM', 'sh-elf-nm'), elf], capture_output=True, text=True, check=True).stdout
    d = {}
    for ln in out.splitlines():
        q = ln.split()
        if len(q) == 3:
            d[q[2]] = int(q[0], 16)
    return d


def main():
    rom = rompatch.apply(open(sys.argv[1], 'rb').read())
    w = json.load(open(sys.argv[2]))
    s = syms(sys.argv[3])
    blob, zero = s['_snes_blob'], s['_zero_page']
    assert s.get('_snes_pg', BASE) == BASE, 'snes_pg must be linked at 0x02300000 (SNES_PG_BANKED)'
    cidx = dict((p, i) for i, p in enumerate(w['common']))
    out = bytearray()
    for bi, pages in enumerate(w['banks']):
        widx = dict((p, j) for j, p in enumerate(pages))
        assert TABLE + len(pages) * 1024 <= BANK, 'world bank %d over 512 KB' % bi
        # (I17) bank 0 also holds the SDRAM game code image (finalize.py, from 0x6F000 - 16)
        assert bi or TABLE + len(pages) * 1024 <= BANK0_END, 'world bank 0 over its %d KB (SDRAM code image after it)' % (BANK0_END // 1024)
        tab = bytearray()
        for p in range(4096):
            if p in widx:
                a = BASE + TABLE + widx[p] * 1024
            elif p in cidx:
                a = blob + cidx[p] * 1024
            else:
                a = zero
            tab += a.to_bytes(4, 'big')
        b = tab + b''.join(rom[p << 10:(p + 1) << 10] for p in pages)
        out += b + b'\xff' * (BANK - len(b))
        print('worldbanks: bank %d (world %d): %d pages, %d KB free' % (
            bi, w['worlds'][bi], len(pages), (BANK - len(b)) // 1024))
    open(sys.argv[4], 'wb').write(out)


if __name__ == '__main__':
    main()
