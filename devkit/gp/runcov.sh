#!/bin/sh
# runcov.sh seed frames entrances...
seed=$1; fr=$2; shift 2
export TRDIR=/opt/work/gp/covd
for e in "$@"; do
  python3 mkplay.py $e $seed $fr > p_$e.txt
  /opt/harness/harness /opt/ref/snes9x_trace/libretro/snes9x_libretro.so /opt/work/dkc.sfc p_$e.txt /opt/work/gp >/dev/null 2>&1
done
