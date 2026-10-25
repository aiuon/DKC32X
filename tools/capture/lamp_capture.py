#!/usr/bin/env python3
"""Capture the hanging-lamp light cone (Winky's Walkway) from the user's ROM in snes9x: one frame
with the lamp on and one with it off ($0579 = 0), the difference is the cone (additive blue).  Also
on/off frames at the start of the other lamp levels (Tanked Up Trouble, Platform Perils) for their
light colour and scene.
usage: lamp_capture.py ROM SNES9X_CORE WORLDMAP_STATE OUT.npz
(personal-use helper; a release tool would rebuild the cone from the BG3 data + HDMA tables)"""
import os as _os, sys as _sys
_sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..'))
import paths
import os, shutil, subprocess, sys, tempfile
import numpy as np
from PIL import Image

rom, core, st, out = sys.argv[1:5]
d = tempfile.mkdtemp(dir=paths.TMP)
shutil.copy(st, os.path.join(d, 'wmap.st'))
open(os.path.join(d, 's.txt'), 'w').write(
    'load wmap.st\npokew 0x40 0xD9\npokew 0x3E 0xD9\nrun 6 START\nrun 60\nunpoke\nrun 300\n'
    'shot on\ndump on.bin 2 0\npokew 0x579 0\nrun 2\nshot off\nunpoke\n')
subprocess.run([paths.HARNESS, core, rom, os.path.join(d, 's.txt'), d],
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
a = np.asarray(Image.open(os.path.join(d, 'on.ppm')).convert('RGB')).astype(np.int64)
b = np.asarray(Image.open(os.path.join(d, 'off.ppm')).convert('RGB')).astype(np.int64)
w = open(os.path.join(d, 'on.bin'), 'rb').read()
u = lambda q: w[q] | w[q + 1] << 8
lx, ly = u(0xDD) - u(0x88B), u(0xDF) - u(0x895)          # lamp on screen (68, 0)
blue = np.clip((a - b)[:, :, 2], 0, 255) // 8             # added blue, 5-bit
# lamp sprite: bright pixels in the lamp box of the 'off' frame (dark ceiling behind it)
box = b[0:30, max(0, lx - 24):lx + 12]
print('lamp at', lx, ly, 'cone max', blue.max())
# the other lamp levels: the light colour differs (Platform Perils: warm), the cone shape is the same
extra = {}
for ent in (0x30, 0x2B):
    d2 = tempfile.mkdtemp(dir=paths.TMP)
    shutil.copy(st, os.path.join(d2, 'wmap.st'))
    open(os.path.join(d2, 's.txt'), 'w').write(
        'load wmap.st\npokew 0x40 0x%X\npokew 0x3E 0x%X\nrun 6 START\nrun 60\nunpoke\nrun 300\n'
        'shot on\npokew 0x579 0\nrun 2\nshot off\nunpoke\n' % (ent, ent))
    subprocess.run([paths.HARNESS, core, rom, os.path.join(d2, 's.txt'), d2],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    extra['on_%02X' % ent] = np.asarray(Image.open(os.path.join(d2, 'on.ppm')).convert('RGB')).astype(np.int16)
    extra['off_%02X' % ent] = np.asarray(Image.open(os.path.join(d2, 'off.ppm')).convert('RGB')).astype(np.int16)
np.savez(out, blue=blue, off=b, lx=lx, ly=ly, on=a, **extra)
