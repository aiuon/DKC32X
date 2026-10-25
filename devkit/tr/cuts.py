# cuts.py DIR: frames where the picture changes a lot (scene cuts), with mean brightness
import sys, glob, os
from PIL import Image, ImageChops, ImageStat
d=sys.argv[1]
fs=sorted(glob.glob(d+'/f*.png'))
prev=None; out=[]
for f in fs:
    im=Image.open(f).convert('L').resize((64,48))
    if prev is not None:
        s=ImageStat.Stat(ImageChops.difference(prev,im)).mean[0]
        if s>12: out.append('%s:%.0f/%.0f'%(os.path.basename(f)[1:7].lstrip('0'),s,ImageStat.Stat(im).mean[0]))
    prev=im
print(' '.join(out))
