#!/bin/bash
# regress.sh NAME [extra flags]: AUTOCYCLE build (every level in turn, the game running), PicoDrive
# shots every 50 frames -> /tmp/claude-0/regress/NAME; diffs against /tmp/claude-0/regress/base
# (changed frames -> NAME_diff.png: old | new).  Deterministic: unchanged code = identical shots.
N=${1:-cur}; F="$2"
D=/tmp/claude-0/regress
mkdir -p $D
/opt/work/bv.sh $D/$N.32x /tmp/claude-0/cap_test/snaps "-DAUTOLEVEL=0 -DNOSCROLL -DAUTOCYCLE=180 $F" || exit 1
python3 -c "
L=['run 150']
for i in range(240): L.append('run 50'); L.append('shot c%03d'%i)
print(';'.join(L))" > $D/script.txt
/opt/work/pd.sh $D/$N.32x $D/$N "$(cat $D/script.txt)" > /dev/null
if [ -d $D/base ] && [ "$N" != base ]; then
python3 - "$D" "$N" <<'PY'
import sys, os
from PIL import Image, ImageChops
D, N = sys.argv[1:3]
ch = []
for f in sorted(os.listdir(D + '/base')):
    if not f.endswith('.png') or not os.path.exists('%s/%s/%s' % (D, N, f)): continue
    a = Image.open('%s/base/%s' % (D, f)).convert('RGB'); b = Image.open('%s/%s/%s' % (D, N, f)).convert('RGB')
    if ImageChops.difference(a, b).getbbox(): ch.append(f)
print('changed frames: %d of 240' % len(ch), ' '.join(c[1:4] for c in ch[:60]))
if ch:
    W = 2 * 160 + 4
    im = Image.new('RGB', (W * 4, 116 * ((min(len(ch), 48) + 3) // 4)))
    for i, f in enumerate(ch[:48]):
        a = Image.open('%s/base/%s' % (D, f)).convert('RGB').resize((160, 112)); b = Image.open('%s/%s/%s' % (D, N, f)).convert('RGB').resize((160, 112))
        x, y = (i % 4) * W, (i // 4) * 116
        im.paste(a, (x, y)); im.paste(b, (x + 162, y))
    im.save('%s/%s_diff.png' % (D, N))
PY
fi
