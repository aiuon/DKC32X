#!/bin/bash
# ab2.sh ENT : load the level on the original ROM, save a state; then the same inputs from that state on
# the original and the patched ROM; compare gameplay WRAM every block
E=$1; D=/tmp/claude-0/bcc/ab2_$E; rm -rf $D; mkdir -p $D/o $D/p
cp wmap.st $D/; printf "load wmap.st\npokew 0x40 0x$E\npokew 0x3E 0x$E\nrun 6 START\nrun 60\nunpoke\nrun 250\nstate lvl.st\n" > $D/s0.txt
(cd $D && /opt/harness/harness_new /opt/ref/snes9x/libretro/snes9x_libretro.so /home/claude/dkc32x/work/rom.sfc s0.txt . > log0.txt 2>&1)
{ echo "load lvl.st"; for i in $(seq 0 59); do echo "run 30 RIGHT"; echo "run 8 RIGHT,B"; echo "run 30 RIGHT,Y"; echo "run 12 LEFT"; echo "run 20 RIGHT,B"; printf "dump w%03d.bin 2 131072\n" $i; done; } > $D/s.txt
for v in o p; do cp $D/lvl.st $D/s.txt $D/$v/; R=/home/claude/dkc32x/work/rom.sfc; [ $v = p ] && R=${PATCHED:-/tmp/claude-0/bcc/rom_all.sfc}
  (cd $D/$v && /opt/harness/harness_new /opt/ref/snes9x/libretro/snes9x_libretro.so $R s.txt . > log.txt 2>&1); done
python3 - $D <<'PY'
import sys
D=sys.argv[1]; bad=0; ent=set()
for i in range(60):
    a=open('%s/o/w%03d.bin'%(D,i),'rb').read(); b=open('%s/p/w%03d.bin'%(D,i),'rb').read()
    w=lambda d,o:d[o]|d[o+1]<<8
    ent.add(w(a,0x3E))
    keys=[k for k in list(range(0xB19,0xE80))+list(range(0x192B,0x1A00))+[0x88B,0x88C,0x895,0x896,0x3E] if a[k]!=b[k]]
    if keys: bad+=1; print('block',i,'differ',len(keys),[hex(k) for k in keys[:8]],'x',w(a,0xB1B),w(b,0xB1B),'ent',hex(w(a,0x3E)),hex(w(b,0x3E))); break
print(D.split('_')[-1], 'OK' if not bad else 'DIVERGED', 'entrances seen', sorted(hex(e) for e in ent))
PY
