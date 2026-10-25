#!/bin/bash
# ares_run.sh ROM PADSCRIPT DUMPDIR FROM TO EVERY QUITAT [mister]: scripted ares run, frames dumped as PNG
R=$1; S=$2; D=$3; F0=$4; F1=$5; EV=$6; Q=$7; M=$8
rm -rf $D; mkdir -p $D
pgrep Xvfb >/dev/null || (Xvfb :99 -screen 0 1280x960x24 >/dev/null 2>&1 &)
sleep 1; export DISPLAY=:99
X=""; [ "$M" = mister ] && X="--setting Developer/ForceInterpreter=true" && export ARES_SDFILL=30 ARES_UNCSD=11
ARES_PADSCRIPT=$S ARES_DUMP=$D ARES_DUMP_FROM=$F0 ARES_DUMP_TO=$F1 ARES_DUMP_EVERY=$EV ARES_QUIT_AT=$Q timeout 3000 /opt/ref/ares/build/rundir/bin/ares --system "Mega 32X" --no-file-prompt --setting Video/Driver="OpenGL 3.2" --setting Video/Blocking=false --setting Input/Defocus=Allow --setting Audio/Blocking=false $X $R > $D/log.txt 2>&1
cd $D && for f in f*.ppm; do convert $f ${f%.ppm}.png && rm $f; done 2>/dev/null
ls $D/*.png 2>/dev/null | wc -l
