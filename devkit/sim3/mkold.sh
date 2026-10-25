#!/bin/sh
# host build of the 32X game runtime (after ./build.sh): sim3/sim ENTRANCE FRAMES
set -e
cd /opt/work/sim3
D=/home/claude/dkc32x
F="${SIMF} -O1 -w -I$D/sh -I$D/tools/recomp -I$D/build/g65 -include /opt/work/sim3/simdefs.h -DR65_NO_GBR"
mkdir -p objvs
for f in $D/build/g65/game65_*.c; do gcc $F -c $f -o objvs/$(basename $f .c).o & done; wait
gcc $F -o sim_old sim.c tramp.c $D/sh/game.c $D/sh/game_hle.c /tmp/claude-0/sc/sprites_old.c -I$D/sh $D/tools/recomp/r65lib.c $D/tools/recomp/r65interp.c $D/build/game/game_tab.c /opt/work/sim/blobs.s objvs/*.o
