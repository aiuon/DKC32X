#!/bin/bash
# allb.sh OUT [flags]: every world in one ROM (window-6 world banks), menu start
cd /home/claude/dkc32x
D=tools/recomp/data
export WORLD_PROFILES="1:$D/memprof.txt:$D/dma_w1.txt,2:$D/memprof_w2.txt:$D/dma_w2.txt,3:$D/memprof_w3.txt:$D/dma_w3.txt,4:$D/memprof_w4.txt:$D/dma_w4.txt,5:$D/memprof_w5.txt:$D/dma_w5.txt,6:$D/memprof_w6.txt:$D/dma_w6.txt,7:$D/memprof_w7.txt:$D/dma_w7.txt"
export WORLD_RESIDENT=$D/memprof_rooms.txt,$D/memprof_maps.txt
export WORLD_CHARS=$D/world_chars.json
MEMPROF=$D/memprof_all.txt SPR_EXCLUDE="" BLOB_FILES="LevelData/SpriteData/,1:Tilemaps/Map32/UnderwaterLevels.bin,3:Tilemaps/Map32/UnderwaterLevels.bin,4:Tilemaps/Map32/UnderwaterLevels.bin,1:LevelData/SnowLevels.bin,1:Tilemaps/Map32/SnowLevels.bin,1:Graphics/Compressed/GFX_FGBG_SnowLevels.bin" EXTRA_SHFLAGS="-DTCACHE $2" GAMEOPT="-DR65_OOL" LEVELS="16 0C 01 BF 17 E0 D9 2E 07 31 42 E1 A5 A4 D0 43 0D DE E5 24 6D A7 3E 14 CE E2 40 2F 18 22 27 41 E3 30 12 0A 36 2B E4 68 D2 A6 D3 6C" ./build.sh /opt/work/dkc.sfc $1 > /tmp/claude-0/b_all.log 2>&1 || { grep -E "Error|error|assert|Traceback" /tmp/claude-0/b_all.log | head -5; tail -6 /tmp/claude-0/b_all.log; exit 1; }
grep -E "game code|world bank|worldbanks|wrote|unclaimed|resident|WORLD_RESIDENT" /tmp/claude-0/b_all.log
