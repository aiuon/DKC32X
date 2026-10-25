#!/bin/bash
# aprof.sh rom.32x out.txt seconds  - ares (interpreter) with per-PC master cycle profile
ROM=$1; OUT=$2; T=${3:-60}; PHASE=${4:-}
pgrep Xvfb >/dev/null || (Xvfb :99 -screen 0 1280x960x24 >/dev/null 2>&1 &)
sleep 2; export DISPLAY=:99
pkill -x ares; sleep 1
rm -f $OUT
env ${EVERY:+ARES_SHPROF_EVERY=$EVERY} ${SLAVE:+ARES_SHPROF_SLAVE=1} ${PHASE:+ARES_PHASE_ADDR=$PHASE} ARES_SHPROF=$OUT /opt/ref/ares/build/rundir/bin/ares --system "Mega 32X" --no-file-prompt --setting Video/Driver="OpenGL 3.2" --setting Video/Blocking=false --setting Input/Defocus=Allow --setting Developer/ForceInterpreter=true "$ROM" > /dev/null 2>&1 &
sleep $T
import -window root -crop 690x530+271+204 ${OUT%.txt}.png
pkill -x ares
