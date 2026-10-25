#!/bin/sh
# mkref.sh G65DIR OUT: a sim (as mkva.sh) from another generated-code directory (regression reference)
set -e
cd /opt/work/sim3
D=/home/claude/dkc32x; G=$1; O=$2
F="-DVRAM_SHADOW  -O1 -w -I$D/sh -I$D/tools/recomp -I$G -include /opt/work/sim3/simdefs.h -DR65_NO_GBR"
mkdir -p obj_$O
for f in $G/game65_*.c; do gcc $F -c $f -o obj_$O/$(basename $f .c).o & done; wait
gcc $F -o $O sim.c tramp.c fehost.c $D/sh/game.c $D/sh/game_hle.c $D/sh/sprites.c $D/tools/recomp/r65lib.c $D/tools/recomp/r65interp.c $D/build/game/game_tab.c /opt/work/sim/blobs.s obj_$O/*.o
