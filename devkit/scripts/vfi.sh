#!/bin/bash
# vfi.sh "ENTS" FRAMES: simva (native) vs simvb FORCEI=1 (all interpreted), WRAM per frame, pad + teleport
ENTS=$1; N=${2:-3000}
for e in $ENTS; do for v in pad tp; do
  if [ $v = pad ]; then X="SCRIPT=/tmp/claude-0/ver/pad.txt"; else X="TP=100,90,96"; fi
  env $X WRAMDUMP=fa_${e}_$v.bin /opt/work/sim3/simva $e $N > /dev/null 2>&1 &
  env $X FORCEI=1 WRAMDUMP=fb_${e}_$v.bin /opt/work/sim3/simvb $e $N > /dev/null 2>&1
  wait
  python3 - fa_${e}_$v.bin fb_${e}_$v.bin $e $v <<'PY'
import sys
a=open(sys.argv[1],'rb').read(); b=open(sys.argv[2],'rb').read()
n=len(a)//len(a) if False else None
FR=len(a)//(len(a)//3000) if False else 0
# per-frame WRAM dumps of 0x20000? find frame size from file length / frames
sz=38656 if len(a)%38656==0 else (len(a)//3000)
frames=len(a)//sz
first=None
for f in range(frames):
    x=a[f*sz:(f+1)*sz]; y=b[f*sz:(f+1)*sz]
    if x!=y:
        d=[i for i in range(sz) if x[i]!=y[i]]
        first=(f,len(d),[hex(i) for i in d[:6]]); break
print(sys.argv[3], sys.argv[4], 'OK' if first is None else 'DIFF frame %d: %d bytes %s'%first, 'size', len(a))
PY
  rm -f fa_${e}_$v.bin fb_${e}_$v.bin
done; done
