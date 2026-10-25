#!/usr/bin/env python3
"""Capture the weather palette keyframes of the snow levels from the user's ROM in snes9x: the level
is entered, then the weather target ($1DF3) is stepped through the states the level visits and the
CGRAM is dumped once each fade (8 steps x 8 frames) has settled; also the VRAM at the heaviest state
(BG3 blizzard map) and the BG register block.
usage: weather_capture.py ROM SNES9X_CORE WORLDMAP_STATE OUT.npz
(personal-use helper, like lamp_capture.py; a release tool would compute the fade targets with the
game's own routines CODE_80A9DF / 80AAF9 / 80AC0B / 80AB3A / 80AB92 / 80ABCF on the ROM palettes)"""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..'))
import paths
import os, shutil, subprocess, sys, tempfile
import numpy as np

rom, core, st, out = sys.argv[1:5]
# entrance -> states in the order the level visits them (the first is the state at level start)
LEVELS = {0x24: [0, 1, 2, 3, 4, 5], 0xA7: [5, 4, 3, 2, 1, 0]}
res = {}
for ent, states in LEVELS.items():
    d = tempfile.mkdtemp(dir=paths.TMP)
    shutil.copy(st, os.path.join(d, 'wmap.st'))
    s = ['load wmap.st', 'pokew 0x40 0x%X' % ent, 'pokew 0x3E 0x%X' % ent, 'run 6 START', 'run 60',
         'unpoke', 'run 300', 'dump c%d.bin 4096 512' % states[0], 'dump w%d.bin 2 0' % states[0]]
    for k in states[1:]:
        s += ['pokew 0x1DF3 %d' % k, 'run 1', 'unpoke', 'run 200',
              'dump c%d.bin 4096 512' % k, 'dump w%d.bin 2 0' % k]
        if k == 5:
            s += ['dump v5.bin 3 65536', 'dump r5.bin 4099 48']
    if states[0] == 5:
        s = s[:9] + ['dump v5.bin 3 65536', 'dump r5.bin 4099 48'] + s[9:]
    open(os.path.join(d, 's.txt'), 'w').write('\n'.join(s) + '\n')
    subprocess.run([paths.HARNESS, core, rom, os.path.join(d, 's.txt'), d],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    pal = np.zeros((6, 256), np.uint16)
    for k in states:
        pal[k] = np.frombuffer(open(os.path.join(d, 'c%d.bin' % k), 'rb').read(), '<u2')
        w = open(os.path.join(d, 'w%d.bin' % k), 'rb').read()
        got = w[0x1DF1] | w[0x1DF2] << 8
        print('%02X state %d: $1DF1 = %d' % (ent, k, got))
        assert got == k or (k == 1 and got in (1, 2)), 'weather state did not settle'
    res['pal_%02X' % ent] = pal
    res['vram5_%02X' % ent] = np.frombuffer(open(os.path.join(d, 'v5.bin'), 'rb').read(), np.uint8)
    res['regs5_%02X' % ent] = np.frombuffer(open(os.path.join(d, 'r5.bin'), 'rb').read(), np.uint8)
# Orang-utan Gang: its darkness trigger fades to a sunset (CODE_81D06A on its palette), with new
# HDMA gradients for the sky ($1B41) and the BG3 haze colour ($1D51)
d = tempfile.mkdtemp(dir=paths.TMP)
shutil.copy(st, os.path.join(d, 'wmap.st'))
open(os.path.join(d, 's.txt'), 'w').write('\n'.join([
    'load wmap.st', 'pokew 0x40 0x0D', 'pokew 0x3E 0x0D', 'run 6 START', 'run 60', 'unpoke', 'run 200',
    'pokew 0x1DF3 2', 'run 1', 'unpoke', 'run 500', 'dump c.bin 4096 512', 'dump w.bin 2 0']) + '\n')
subprocess.run([paths.HARNESS, core, rom, os.path.join(d, 's.txt'), d],
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
w = open(os.path.join(d, 'w.bin'), 'rb').read()
res['sunset_0D_pal'] = np.frombuffer(open(os.path.join(d, 'c.bin'), 'rb').read(), '<u2')
res['sunset_0D_sky'] = np.frombuffer(w[0x1B41:0x1B41 + 0x220], np.uint8)
res['sunset_0D_bg3'] = np.frombuffer(w[0x1D51:0x1D51 + 0xA0], np.uint8)
np.savez(out, **res)
print('saved', out)
