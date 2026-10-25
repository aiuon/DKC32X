# blk.py EVENTSDIR: black periods from the kept frames' events (mean < 3), sheets of their ends
import sys, os, glob
from PIL import Image, ImageStat
d = sys.argv[1]
ev = [l.split() for l in open(d + '/events.txt')]
ends = sorted(set(int(e[0]) for e in ev if e[1] == 'cut' and float(e[3]) > 5 and any(int(x[0]) == int(e[0]) - 0 for x in ev)))
# a 'cut' whose previous frame is black
out = []
for f in ends:
    p = d + '/f%06d.png' % (f - 1)
    if os.path.exists(p) and ImageStat.Stat(Image.open(p).convert('L')).mean[0] < 3: out.append(f)
print(len(out), out)
for k in range(0, len(out), 6):
    fs = []
    for f in out[k:k + 6]:
        for j in range(f - 2, f + 8):
            p = d + '/f%06d.png' % j
            fs.append(p if os.path.exists(p) else None)
    W, H = 160, 121
    im = Image.new('RGB', (W * 10, H * 6))
    for i, p in enumerate(fs):
        if p: im.paste(Image.open(p).convert('RGB').resize((W, H)), ((i % 10) * W, (i // 10) * H))
    im.save('%s/sheet_%02d.png' % (d, k // 6))
