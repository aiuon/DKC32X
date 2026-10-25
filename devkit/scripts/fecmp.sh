#!/bin/bash
# fecmp.sh: host renders of the ending (speech / hoard / credits) and the boot screens, sim_ref vs sim_xc
cd /opt/work/sim3
R=${FEROM:-/tmp/claude-0/pf_dbg2.32x}
run() { b=$1; tag=$2; envs=$3; shift 3; rm -rf /tmp/claude-0/feh; mkdir -p /tmp/claude-0/feh; env FEHOST=$R FEVM=1 FECOUNT=1 $envs timeout 1500 ./sim_$b "$@" > /tmp/claude-0/fc_${tag}_$b.log 2>&1; rm -rf /tmp/claude-0/fc_${tag}_$b; mv /tmp/claude-0/feh /tmp/claude-0/fc_${tag}_$b; }
for b in ${BINS-ref xc}; do
  run $b end "FEEVERY=3 FESHOT=$(seq -s, 33 33 990) JUMPF=30:00B884D7" 68 1000
  run $b cred "FEEVERY=2 FESHOT=$(seq -s, 1950 50 4000) JUMPF=30:00B884D7" 68 4000
  run $b boot "FEEVERY=2 FESHOT=$(seq -s, 20 20 1300) JUMP0=0080BA2C SCRIPT=/tmp/claude-0/nav_s1.txt" 100 1300
done
for t in end cred boot; do n=0; d=0; for f in /tmp/claude-0/fc_${t}_ref/*.ppm; do n=$((n+1)); cmp -s $f /tmp/claude-0/fc_${t}_xc/$(basename $f) || { d=$((d+1)); echo "  DIFF $t $(basename $f)"; }; done; echo "$t: $n shots, $d differ"; done
