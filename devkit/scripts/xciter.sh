#!/bin/bash
# translate the front-end continuation keys until none are left (sim_xc XC_PROF over the menu script)
cd /home/claude/dkc32x
for it in $(seq 1 20); do
  n=$(awk '$3==0' /tmp/claude-0/fg/xcl.txt | wc -l)
  echo "iter $it: $n untranslated"
  [ "$n" = 0 ] && break
  awk '$3==0 {print $1}' /tmp/claude-0/fg/xcl.txt >> tools/recomp/data/translate.txt
  python3 tools/recomp/r65.py build/rom.sfc tools/recomp/data build/g65 --translate tools/recomp/data/translate.txt --files 8 --sdram tools/recomp/data/sdram.txt --hot tools/recomp/data/hot.txt --prof tools/recomp/data/pcprof.txt 2>&1 | tail -1
  (cd /opt/work/sim3 && SIMF="-DVRAM_SHADOW -DWH_CHECK -DROMB_CHECK -DXC_PROF" sh mkxc.sh 2>&1 | grep -i " error")
  (cd /tmp/claude-0/fg && XCFILE=/tmp/claude-0/fg/xcl.txt JUMP0=0080BA2C SCRIPT=menu.txt /opt/work/sim3/sim_xc 100 2600 >/dev/null 2>&1)
done
