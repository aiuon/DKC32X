#!/bin/bash
# arun2.sh ROM OUT.png : ares interpreter MiSTer model; screenshots from 70 s every 10 s until the HUD
# (white text, top left) is visible -> OUT.png (HUD crop) + OUT_full.png
R=$1; O=$2
pgrep Xvfb >/dev/null || (Xvfb :99 -screen 0 1280x960x24 >/dev/null 2>&1 &)
sleep 1; export DISPLAY=:99; pkill -x ares; sleep 1
ARES_SDFILL=30 ARES_UNCSD=11 /opt/ref/ares/build/rundir/bin/ares --system "Mega 32X" --no-file-prompt --setting Video/Driver="OpenGL 3.2" --setting Video/Blocking=false --setting Input/Defocus=Allow --setting Developer/ForceInterpreter=true $R > /dev/null 2>&1 &
sleep 70
for t in 1 2 3 4 5 6 7 8 9; do
  import -window root -crop 690x530+271+204 ${O%.png}_full.png
  if python3 -c "
from PIL import Image
im=Image.open('${O%.png}_full.png').convert('RGB').crop((0,0,400,72))
px=list(im.crop((0,0,260,60)).getdata())
n=sum(1 for p in px if p[0]>230 and p[1]>230 and p[2]>230); k=sum(1 for p in px if max(p)<12)
im.save('$O')
raise SystemExit(0 if n>400 and k>len(px)*0.35 else 1)"; then break; fi
  sleep 10
done
pkill -x ares
