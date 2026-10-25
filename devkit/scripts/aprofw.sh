#!/bin/bash
# aprofw.sh rom.32x outdir seconds interval : ares interpreter profile, snapshots of the cumulative
# profile + a screenshot every interval (diff successive snapshots for a time window)
ROM=$1; O=$2; T=${3:-120}; IV=${4:-5}
rm -rf $O; mkdir -p $O
pgrep Xvfb >/dev/null || (Xvfb :99 -screen 0 1280x960x24 >/dev/null 2>&1 &)
sleep 2; export DISPLAY=:99
pkill -x ares; sleep 1
env ${EVERY:+ARES_SHPROF_EVERY=$EVERY} ${SLAVE:+ARES_SHPROF_SLAVE=1} ARES_SHPROF=$O/prof.txt /opt/ref/ares/build/rundir/bin/ares --system "Mega 32X" --no-file-prompt --setting Video/Driver="OpenGL 3.2" --setting Video/Blocking=false --setting Input/Defocus=Allow --setting Developer/ForceInterpreter=true "$ROM" > /dev/null 2>&1 &
t=0
while [ $t -lt $T ]; do sleep $IV; t=$((t+IV)); [ -f $O/prof.txt ] && cp $O/prof.txt $O/p_$t.txt; import -window root -crop 690x530+271+204 $O/s_$t.png; done
pkill -x ares
