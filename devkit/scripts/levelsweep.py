import json, subprocess, os, sys
from concurrent.futures import ThreadPoolExecutor
L=json.load(open('/home/claude/dkc32x/tools/levels.json'))
WORLDS=['Kongo Jungle','Monkey Mines','Vine Valley','Gorilla Glacier','Kremkroc Industries','Chimp Caverns']
jobs=[]
for lv in L:
    if lv.get('room'): continue
    w=lv.get('world')
    wi=WORLDS.index(w) if w in WORLDS else (6 if 'Galleon' in lv['name'] else None)
    if wi is None: continue
    jobs.append((lv['entrance'], wi, lv['x_min'], lv['x_max'], lv['name']))
def run(j):
    e,wi,x0,x1,n=j
    dx=64; ny=8; npts=((x1-x0)//dx+1)*ny; frames=min(30+npts*3+10, 9000)
    env=dict(os.environ, WORLDBANK=str(wi), TPGRID='%d,%d,%d,%d,%d,%d,3'%(x0,x1,dx,24,24+64*(ny-1),64))
    r=subprocess.run(['/opt/work/sim3/sim_xcp','%02X'%e,str(frames)],env=env,capture_output=True,text=True,cwd='/tmp/claude-0/sweep15')
    out=[l for l in (r.stdout+r.stderr).splitlines() if any(k in l for k in ('MISSING','WORLD page','SCRATCH','dummy','err 2','STUCK','fatal'))]
    return j,frames,out
with ThreadPoolExecutor(2) as ex:
    for (e,wi,x0,x1,n),fr,out in ex.map(run,jobs):
        print('%02X %-28s w%d frames %d: %d issues'%(e,n,wi+1,fr,len(out)),flush=True)
        for l in out[:12]: print('   ',l,flush=True)
