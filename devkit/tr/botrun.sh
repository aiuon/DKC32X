#!/bin/bash
# botrun.sh ROM TAG FRAMES: ares MiSTer-model run, every frame dumped (half size), analyzed in chunks
R=$1; T=$2; N=$3
D=/tmp/claude-0/tr/$T; E=/tmp/claude-0/tr/${T}_ev; rm -rf $D $E; mkdir -p $D $E
export DISPLAY=:99; pgrep Xvfb >/dev/null || (Xvfb :99 -screen 0 1280x960x24 >/dev/null 2>&1 &); sleep 1
(ARES_SDFILL=30 ARES_UNCSD=11 ARES_DUMP=$D ARES_DUMP_HALF=1 ARES_QUIT_AT=$N timeout 14000 /opt/ref/ares/build/rundir/bin/ares --system "Mega 32X" --no-file-prompt --setting Video/Driver="OpenGL 3.2" --setting Video/Blocking=false --setting Audio/Blocking=false --setting Developer/ForceInterpreter=true --setting Input/Defocus=Allow $R > /tmp/claude-0/tr/$T.log 2>&1; touch $D.done) &
k=0
while true; do
  n=$(ls $D | wc -l)
  if [ -f $D.done ] || [ $n -gt 3000 ]; then
    mkdir -p $D.c$k; ls $D | sort | head -n $( [ -f $D.done ] && echo $n || echo 2990 ) | while read f; do mv $D/$f $D.c$k/; done
    python3 /tmp/claude-0/tr/ana.py $D.c$k $E > /dev/null; rm -rf $D.c$k; k=$((k+1))
    [ -f $D.done ] && [ $(ls $D | wc -l) -eq 0 ] && break
  fi
  sleep 20
done
echo botrun-done
