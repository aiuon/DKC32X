import sys; sys.path.insert(0,'/home/claude/dkc32x/tools/recomp')
from r65 import *
rom=Rom('/home/claude/dkc32x/work/rom.sfc')
pc=int(sys.argv[1],16); mode=int(sys.argv[2]); n=int(sys.argv[3])
for _ in range(n):
    i=decode(rom,pc,mode)
    print('%06X %-4s %s %0*X'%(pc,i.mn,i.md,2*i.n,i.arg) if i.n else '%06X %-4s %s'%(pc,i.mn,i.md))
    if i.mn=='REP': mode&=~((i.arg>>4)&3)  # bits: M=0x20->2, X=0x10->1
    if i.mn=='SEP': mode|=((i.arg>>4)&3)
    pc=nextpc(pc,1+i.n)
