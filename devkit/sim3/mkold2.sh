#!/bin/sh
# host build of the 32X game runtime (after ./build.sh): sim3/sim ENTRANCE FRAMES
set -e
cd /opt/work/sim3
D=/home/claude/dkc32x
F="${SIMF--DVRAM_SHADOW -DWH_CHECK -DROMB_CHECK} -O1 -w -I$D/sh -I$D/tools/recomp -I/tmp/claude-0/g65_old -include /opt/work/sim3/simdefs.h -DR65_NO_GBR"
mkdir -p objold
for f in /tmp/claude-0/g65_old/game65_*.c; do gcc $F -c $f -o objold/$(basename $f .c).o & done; wait
gcc $F -o sim_old2 sim.c tramp.c fehost.c $D/sh/game.c $D/sh/game_hle.c $D/sh/sprites.c $D/tools/recomp/r65lib.c $D/tools/recomp/r65interp.c $D/build/game/game_tab.c /opt/work/sim/blobs.s objold/*.o
