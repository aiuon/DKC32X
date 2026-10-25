# ana.py DIR OUT : scene cuts / one-frame flashes in a dump of f*.ppm (deleted afterwards); frames
# around each event saved as PNG in OUT, a log of events in OUT/events.txt
import sys, os, glob, numpy as np
from PIL import Image
d, out = sys.argv[1], sys.argv[2]; os.makedirs(out, exist_ok=True)
fs = sorted(glob.glob(d + '/f*.ppm'))
def load(f):
    with open(f, 'rb') as fp:
        data = fp.read()
    # P6\nW H\n255\n
    parts = data.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    a = np.frombuffer(parts[3][:w*h*3], dtype=np.uint8).reshape(h, w, 3)
    return a
ev = []; prev = None; pprev = None; keep = set()
small = []
for i, f in enumerate(fs):
    a = load(f); g = a.mean(axis=2)[::4, ::4]
    small.append(g)
    if prev is not None:
        dc = np.abs(g - prev).mean()
        if dc > 12: ev.append((i, 'cut', dc, g.mean()))
        if pprev is not None:
            fl = min(np.abs(prev - pprev).mean(), dc) - np.abs(g - pprev).mean()
            if fl > 6: ev.append((i - 1, 'flash', fl, prev.mean()))
    pprev, prev = prev, g
for i, k, s, m in ev:
    for j in range(max(0, i - 6), min(len(fs), i + 7)): keep.add(j)
for j in sorted(keep):
    Image.fromarray(load(fs[j])).save(os.path.join(out, os.path.basename(fs[j])[:-4] + '.png'))
with open(os.path.join(out, 'events.txt'), 'a') as o:
    for i, k, s, m in ev: o.write('%s %s %.0f %.0f\n' % (os.path.basename(fs[i])[1:7], k, s, m))
for f in fs: os.remove(f)
print(len(fs), 'frames,', len(ev), 'events')
