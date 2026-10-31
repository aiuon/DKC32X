# tilechk.py ROM32 ELF SNESROM LOGDIR: OBJ tiles that cross a 1 KB page boundary, read the way the 32X does
# (world bank table / resident blob / straddle windows) vs the SNES ROM bytes, for the DMA logs in LOGDIR
import json, subprocess, struct, sys, os
rom32=open(sys.argv[1],'rb').read(); elf=sys.argv[2]
snes=open(sys.argv[3],'rb').read(); snes=snes[0x200:] if len(snes)%0x8000==0x200 else snes
D='/home/claude/dkc32x'; wb=open(D+'/build/gen2/worldbanks.bin','rb').read()
S={l.split()[2]:int(l.split()[0],16) for l in subprocess.run(['sh-elf-nm',elf],capture_output=True,text=True).stdout.splitlines() if len(l.split())==3}
def rd(addr,n,bank):
    if 0x02300000<=addr<0x02380000: o=bank*0x80000+addr-0x02300000; return wb[o:o+n]
    return rom32[addr-0x02000000:addr-0x02000000+n]
N=struct.unpack('>H',rd(S['_game_straddle_n'],2,0))[0]
PG=[struct.unpack('>H',rd(S['_game_straddle_pg']+2*i,2,0))[0] for i in range(N)]
w=json.load(open(D+'/build/game/worlds.json')); L=json.load(open(D+'/tools/levels.json'))
Wn=['Kongo Jungle','Monkey Mines','Vine Valley','Gorilla Glacier','Kremkroc Industries','Chimp Caverns']
tab=lambda a,b: struct.unpack('>I',wb[b*0x80000+(a>>10)*4:b*0x80000+(a>>10)*4+4])[0]
def tile(a,b,fix):
    if fix and (a>>10) in PG: return rd(S['_game_straddle_win']+PG.index(a>>10)*64+(a&0x3FF)-0x3E0,32,b)
    return rd(tab(a,b)+(a&0x3FF),32,b)
tot=bo=bn=0; bad={}
for l in L:
    if l.get('room'): continue
    e='%02X'%l['entrance']; f=os.path.join(sys.argv[4],e+'.txt')
    if not os.path.exists(f): continue
    b=w['worlds'].index(Wn.index(l['world'])+1 if l['world'] in Wn else 7)
    for ln in set(open(f).read().split('\n')):
        q=ln.split()
        if len(q)!=2: continue
        src=int(q[0],16); n=int(q[1],16)
        if (src>>16)&0xFE==0x7E or n>=0x2000: continue
        for i in range(n//32):
            a=(src+i*32)&0x3FFFFF
            if (a&0x3FF)<=0x3E0 or tab(a,b)==S['_zero_page']: continue
            tot+=1; ref=snes[a:a+32]
            bo+=tile(a,b,False)!=ref
            if tile(a,b,True)!=ref: bn+=1; bad.setdefault(hex(src),set()).add(e)
print('straddling tiles',tot,'wrong without windows',bo,'wrong now',bn); print({k:len(v) for k,v in bad.items()})
