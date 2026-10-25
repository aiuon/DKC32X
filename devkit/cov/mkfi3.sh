#!/bin/sh
# sim_fc: translated-function call counts (FCFILE=out) + interpreted keys (XCFILE=out)
set -e
cd /opt/work/sim3
D=/home/claude/dkc32x
F="-DVRAM_SHADOW -DROMB_CHECK -DWH_CHECK -DXC_PROF -DR65_FORCEI -O1 -w -I$D/sh -I$D/tools/recomp -I$D/build/g65 -include /opt/work/sim3/simdefs.h -DR65_NO_GBR"
mkdir -p objfi3
for f in $D/build/g65/game65_*.c; do gcc $F -c $f -o objfi3/$(basename $f .c).o & done; wait
gcc $F -o sim_fi3 sim.c tramp.c fehost.c $D/sh/game.c $D/sh/game_hle.c $D/sh/sprites.c $D/tools/recomp/r65lib.c $D/tools/recomp/r65interp.c $D/build/game/game_tab.c /opt/work/sim/blobs.s objfi3/*.o
