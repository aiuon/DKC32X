#!/bin/bash
# verify2.sh "ENTS" FRAMES REFSIM : new sim (simva) vs REFSIM (default simva_orig), WRAM per frame
cd /tmp/claude-0/ver
ENTS=${1:-"16 0C 01 BF 17 E0 D9 2E 07 31 42"}; N=${2:-1500}; R=${3:-/opt/work/sim3/simva_orig}
for e in $ENTS; do
  for v in pad tp; do
    if [ $v = pad ]; then X="SCRIPT=/tmp/claude-0/ver/pad.txt"; else X="TP=100,90,96"; fi
    env $X WRAMDUMP=a_${e}_$v.bin /opt/work/sim3/simva $e $N > a_${e}_$v.log 2>&1 &
    env $X WRAMDUMP=o_${e}_$v.bin $R $e $N > o_${e}_$v.log 2>&1
    wait
    if cmp -s a_${e}_$v.bin o_${e}_$v.bin; then echo "$e $v OK $(stat -c %s a_${e}_$v.bin)"; rm -f a_${e}_$v.bin o_${e}_$v.bin b_${e}_$v.bin; else echo "$e $v DIFF $(cmp a_${e}_$v.bin o_${e}_$v.bin | head -1)"; fi
  done
done
