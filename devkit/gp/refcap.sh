#!/bin/sh
# refcap.sh entrance seed frames -> ref/<e>_<seed>.wai
e=$1; seed=$2; fr=$3
python3 mkplay.py $e $seed $fr | sed 's/^dump x 0x1010 1$//' | awk -v OFS='\n' '/^run 120$/{print; print "dump x 0x1010 1"; next} {print}' > rp_$e.txt
TRWAI=cb TRWAIFILE=/opt/work/gp/ref/${e}_${seed}.wai TRDIR=/opt/work/gp/covd_extra /opt/harness/harness /opt/ref/snes9x_trace/libretro/snes9x_libretro.so /opt/work/dkc.sfc rp_$e.txt /opt/work/gp >/dev/null 2>&1
