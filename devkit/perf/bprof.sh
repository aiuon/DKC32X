#!/bin/bash
# bprof.sh ROM ELF LEVEL QUIT OUT [slave] : per-PC profile (bus model, real-hardware knobs) while in_level != 0
R=$1; E=$2; L=$3; Q=$4; O=$5; SL=$6
off=$(( 0x$(sh-elf-nm $E | awk '$3=="_perf_cfg"{print $1}') - 0x02000000 ))
ph=$(printf "%08x" $(( 0x$(sh-elf-nm $E | awk '$3=="_game_on"{print $1}') + 2 )))
T=/tmp/claude-0/fpl/bprof_$L$SL.32x; cp $R $T
python3 -c "
import struct; d=bytearray(open('$T','rb').read()); o=$off
d[o+4:o+16]=struct.pack('>III',$L,0,0); open('$T','wb').write(d)"
export DISPLAY=:99; rm -f $O
env ${SL:+ARES_SHPROF_SLAVE=1} ARES_SHPROF=$O ARES_SHPROF_EVERY=${EVERYC:-200000000} ARES_PHASE_ADDR=$ph ARES_BUS=${ARES_BUS-1} ARES_QUIT_AT=$Q ARES_ROMW=${ROMW:-12} ARES_FBR=8 ARES_SDFILL=12 ARES_UNCSD=10 timeout 1500 /opt/ref/ares/build/rundir/bin/ares --system "Mega 32X" --no-file-prompt --setting Video/Driver="OpenGL 3.2" --setting Video/Blocking=false --setting Audio/Blocking=false --setting Input/Defocus=Allow --setting Developer/ForceInterpreter=true $T > /dev/null 2>&1
