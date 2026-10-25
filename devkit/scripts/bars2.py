import sys
from PIL import Image
names=['vbl','gf','lines','full','part','copy','setup','tline','tsig','n','def','p_game','p_setup','p_loop','p_spr','p_rest','p_pres']
scale=[8,8,0.25,0.25,0.25,0.25,1,16,16,0.25,0.25,16,16,16,16,16,16]
for fn in sys.argv[1:]:
    im=Image.open(fn).convert('RGB'); W,H=im.size; p=im.load()
    rows=[y for y in range(H) if sum(p[W//2,y])>30]
    if not rows: print(fn,'black'); continue
    y0=rows[0]; sy=(rows[-1]-y0+1)/224.0
    # left edge of the 32X screen: first column with any content over the whole height, else assume
    cols=[x for x in range(W) if any(sum(p[x,yy])>30 for yy in range(y0,rows[-1],4))]
    sx=2.156; x0=cols[0]
    out=[]
    for k in range(17):
        y=int(y0+(1+2*k+0.5)*sy); n=0
        while n<32 and min(p[x0+int((n+0.5)*sx),y])>240: n+=1
        out.append(n/scale[k])
    print(fn.split('/')[-1], ' '.join('%s %.2f'%(a,b) if isinstance(b,float) and b!=int(b) else '%s %d'%(a,b) for a,b in zip(names,out)))
