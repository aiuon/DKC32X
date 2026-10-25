import sys, os, glob
from PIL import Image, ImageChops, ImageStat
d=sys.argv[1]
for seg in 'ABCD':
    fs=sorted(glob.glob('%s/%s*.png'%(d,seg)))
    ims=[Image.open(f).convert('L').resize((80,56)) for f in fs]
    def df(a,b): return ImageStat.Stat(ImageChops.difference(a,b)).mean[0]
    sc=[]
    for i in range(1,len(ims)-1):
        s=df(ims[i-1],ims[i])+df(ims[i],ims[i+1])-df(ims[i-1],ims[i+1])
        sc.append((s,i))
    sc.sort(reverse=True)
    print(seg, len(fs), ' '.join('%d:%.0f'%(i,s) for s,i in sc[:8]))
