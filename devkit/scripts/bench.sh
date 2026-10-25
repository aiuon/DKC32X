#!/bin/bash
# bench.sh TAG "IDX..." [extra flags]: GAMEDEMO+GAMEPROF builds per level, ares interpreter with the MiSTer
# memory model (SDRAM fill 30, uncached SDRAM 11), HUD crop after frame 360 -> /tmp/claude-0/bench/TAG_IDX.png
TAG=$1; IDXS=$2; F="$3"
D=/tmp/claude-0/bench; mkdir -p $D
for i in $IDXS; do
  /opt/work/allb.sh $D/${TAG}_$i.32x "-DGAMEDEMO -DGAMEPROF -DNO_FSKIP -DAUTOLEVEL=$i $F" > $D/${TAG}_$i.blog 2>&1 || { echo "build $i failed"; tail -3 $D/${TAG}_$i.blog; continue; }
  cp /home/claude/dkc32x/build/sh.elf $D/${TAG}_$i.elf
done
for i in $IDXS; do
  [ -f $D/${TAG}_$i.32x ] || continue
  /tmp/claude-0/arun2.sh $D/${TAG}_$i.32x $D/${TAG}_$i.png
done
cd $D && montage $(for i in $IDXS; do echo ${TAG}_$i.png; done) -tile 1x -geometry +0+2 ${TAG}_all.png
