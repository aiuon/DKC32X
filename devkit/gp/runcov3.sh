#!/bin/sh
seed=$1; fr=$2; shift 2
for e in "$@"; do
  python3 mkplay2.py $e $seed $fr > q_$e.txt
  /opt/harness/harness /opt/ref/snes9x_trace/libretro/snes9x_libretro.so /opt/work/dkc.sfc q_$e.txt /opt/work/gp >/dev/null 2>&1
done
