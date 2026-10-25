#!/bin/sh
# capsnap.sh entrance [frames-after-start] -> snaps/<e>.wai (WAI-point snapshots, last one used)
#   + snaps/<e>.vram (64 KB), .cgram (512), .regs (FillRAM 0x2100-0x21FF, 0x4200-0x43FF)
e=$1; n=${2:-150}
cat > cs_$e.txt <<EOT
load ../sg/wmap.st
pokew 0x40 $((0x$e))
pokew 0x3E $((0x$e))
run 6 START
run 60
unpoke
run $n
dump x 0x1010 1
run 3
dump z 0x1016 8
dump snaps/$e.vram 3 65536
dump snaps/$e.cgram 4096 512
dump snaps/$e.fill 4097 32768
EOT
rm -f snaps/$e.wai
TRWAI=cb TRWAIFILE=/opt/work/gp/snaps/$e.wai /opt/harness/harness /opt/ref/snes9x_trace/libretro/snes9x_libretro.so /opt/work/dkc.sfc cs_$e.txt /opt/work/gp >/dev/null 2>&1
