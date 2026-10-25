#!/bin/sh
# sim_fc: translated-function call counts (FCFILE=out) + interpreted keys (XCFILE=out)
set -e
cd /opt/work/sim3
D=/home/claude/dkc32x
F="-DVRAM_SHADOW -DROMB_CHECK -DWH_CHECK -DXC_PROF -DFCOUNT_PROF -O1 -w -I$D/sh -I$D/tools/recomp -I/tmp/claude-0/g65_base -DR65_NO_JMLHANDOFF -include /opt/work/sim3/simdefs.h -DR65_NO_GBR"
mkdir -p objfcb
for f in /tmp/claude-0/g65_base/game65_*.c; do gcc $F -c $f -o objfcb/$(basename $f .c).o & done; wait
gcc $F -o sim_fcb sim.c tramp.c fehost.c $D/sh/game.c $D/sh/game_hle.c $D/sh/sprites.c $D/tools/recomp/r65lib.c $D/tools/recomp/r65interp.c $D/build/game/game_tab.c /opt/work/sim/blobs.s objfcb/*.o
