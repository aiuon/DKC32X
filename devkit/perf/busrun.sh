#!/bin/bash
# busrun.sh ROM ELF LEVEL QUIT [EVERY] : PERFREC ROM at a level, ares real-hardware model + bus model, bus stats
R=$1; E=$2; L=$3; Q=${4:-1200}; EV=${5:-120}
off=$(( 0x$(sh-elf-nm $E | awk '$3=="_perf_cfg"{print $1}') - 0x02000000 ))
T=/tmp/claude-0/fpl/busrun_$L.32x; cp $R $T
python3 -c "
import struct,sys; d=bytearray(open('$T','rb').read()); o=$off
assert struct.unpack('>I',d[o:o+4])[0]==0x50434647; d[o+4:o+16]=struct.pack('>III',$L,0,0); open('$T','wb').write(d)"
export DISPLAY=:99
ARES_BUS=${ARES_BUS-1} ARES_BUSSTAT=$EV ARES_QUIT_AT=$Q ARES_ROMW=${ROMW:-12} ARES_FBR=8 ARES_SDFILL=12 ARES_UNCSD=10 timeout 900 /opt/ref/ares/build/rundir/bin/ares --system "Mega 32X" --no-file-prompt --setting Video/Driver="OpenGL 3.2" --setting Video/Blocking=false --setting Audio/Blocking=false --setting Input/Defocus=Allow --setting Developer/ForceInterpreter=true $T 2>&1 | grep -A2 "^BUS" 
