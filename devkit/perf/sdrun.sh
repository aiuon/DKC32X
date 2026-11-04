#!/bin/bash
# sdrun.sh ROM ELF LEVEL QUIT SDOUT [bus=1] : PERFREC level run (or LEVEL=-1: no patch) in the real-hardware model, SDRAM dump at the end
R=$1; E=$2; L=$3; Q=$4; SD=$5; B=${6:-1}; [ -f "$R" ] || { echo "no ROM $R"; exit 1; }
T=/tmp/claude-0/fpl/sdrun_$$.32x; cp $R $T
if [ "$L" != "-1" ]; then off=$(( 0x$(sh-elf-nm $E | awk '$3=="_perf_cfg"{print $1}') - 0x02000000 ))
python3 -c "
import struct; d=bytearray(open('$T','rb').read()); o=$off
d[o+4:o+16]=struct.pack('>III',$L,0,0); open('$T','wb').write(d)"; fi
export DISPLAY=:99; rm -f $SD
ARES_SDDUMP=$SD ARES_BUS=$B ARES_QUIT_AT=$Q ARES_ROMW=${ROMW:-12} ARES_FBR=8 ARES_SDFILL=12 ARES_UNCSD=10 ${PADS:+ARES_PADSCRIPT=$PADS} timeout 1500 /opt/ref/ares/build/rundir/bin/ares --system "Mega 32X" --no-file-prompt --setting Video/Driver="OpenGL 3.2" --setting Video/Blocking=false --setting Audio/Blocking=false --setting Input/Defocus=Allow --setting Developer/ForceInterpreter=true $T > /dev/null 2>&1
rm -f $T
