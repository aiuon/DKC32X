#!/bin/bash
# roomtest.sh ROOM [shots] [interval]: Barrel Cannon Canyon, idle until the first death (map), then enter
# entrance ROOM from the map (as the survey did) -> build/rt_ROOM.32x, ares shots /tmp/claude-0/rm/ROOM_N.png
R=$1; N=${2:-16}; IV=${3:-0.8}
P=""; for f in $(seq 396 408); do P="$P,$f,0x40,0x$R,$f,0x3E,0x$R"; done; P=${P#,}
/opt/work/wb.sh /home/claude/dkc32x/build/rt_$R.32x "-DAUTOLEVEL=4 -DDBG_POKE=$P -DDBG_PAD=400,406,0x1000 $XF" || exit 1
mkdir -p /tmp/claude-0/rm
timeout 300 /opt/work/shots32x.sh /home/claude/dkc32x/build/rt_$R.32x /tmp/claude-0/rm/${R}_ 12 $N $IV >/dev/null 2>&1
python3 -c "
from PIL import Image
n=$N; ims=[Image.open('/tmp/claude-0/rm/${R}_%d.png'%i).resize((345,265)) for i in range(n)]
W=Image.new('RGB',(345*6,265*((n+5)//6)))
for i,im in enumerate(ims): W.paste(im,((i%6)*345,(i//6)*265))
W.save('/tmp/claude-0/rm/${R}_grid.png')"
