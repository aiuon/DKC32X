#!/bin/sh
# regenerate + build host runner; args passed to r65.py
set -e
cd /home/claude/dkc32x/tools/recomp
python3 r65.py /opt/work/dkc.sfc /opt/work/gp/covd /opt/work/gp/gen "$@"
cd /opt/work/gp/gen
rm -rf obj; mkdir -p obj
ls game65_*.c | xargs -P 4 -I{} sh -c 'gcc -O1 -w -DR65_HOST -DR65_TRACE -DR65_MEMTRACK -I/home/claude/dkc32x/tools/recomp -I. -c {} -o obj/{}.o'
cd /opt/work/gp
gcc -O1 -w -DR65_HOST -DR65_TRACE -DR65_MEMTRACK -I/home/claude/dkc32x/tools/recomp -o hostrun /home/claude/dkc32x/tools/recomp/host/hostrun.c /home/claude/dkc32x/tools/recomp/r65lib.c /home/claude/dkc32x/tools/recomp/r65interp.c /home/claude/dkc32x/sh/game_hle.c gen/obj/*.o
