# fscan.py DIR [N]: one-frame flashes (a frame unlike both neighbours) -> top N frame numbers
import sys, glob, os
from PIL import Image, ImageChops, ImageStat
d=sys.argv[1]; N=int(sys.argv[2]) if len(sys.argv)>2 else 25
fs=sorted(glob.glob(d+'/f*.png'))
ims=[Image.open(f).convert('L').resize((64,48)) for f in fs]
def df(a,b): return ImageStat.Stat(ImageChops.difference(a,b)).mean[0]
sc=[]
for i in range(1,len(ims)-1):
    a=df(ims[i-1],ims[i]); b=df(ims[i],ims[i+1]); c=df(ims[i-1],ims[i+1])
    sc.append((min(a,b)-c, os.path.basename(fs[i])[1:7]))
sc.sort(reverse=True)
print(' '.join('%s:%.0f'%(f,s) for s,f in sc[:N]))
