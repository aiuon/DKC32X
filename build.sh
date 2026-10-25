#!/bin/sh
# Build the DKC 32X viewer from your own Donkey Kong Country (USA v1.0) ROM.
#   ./build.sh path/to/dkc.sfc [output.32x]
set -e
ROM="$1"; OUT="${2:-build/dkc_viewer.32x}"
[ -f "$ROM" ] || { echo "usage: $0 <Donkey Kong Country (USA).sfc> [out.32x]"; exit 1; }
cd "$(dirname "$0")"
SHCC=${SHCC:-/opt/sh-elf/bin/sh-elf-gcc}
MDCC=${MDCC:-m68k-linux-gnu-gcc}
mkdir -p build/gen2

# game logic: recompiled 65816 (tools/recomp), SNES data pages, level-start snapshots
SNAPDIR=${SNAPDIR:-/opt/work/gp/snaps}
mkdir -p build/g65 build/game
python3 tools/recomp/r65.py "$ROM" tools/recomp/data build/g65 --translate tools/recomp/data/translate.txt --files 8 --sdram "${SDRAMLIST:-tools/recomp/data/sdram.txt}" --hot "${HOTLIST:-tools/recomp/data/hot.txt}" --prof "${PCPROF:-tools/recomp/data/pcprof.txt}"
# sprite characters never seen in world 1 (ROM space; the full game will need them)
export SPR_EXCLUDE="${SPR_EXCLUDE-KRool,KRoolCrown,KRoolsShip,QueenB,MasterNecky,MasterNeckysNut,MankyKong,Krusha,Minecart,MinecartSparks,Rockkroc,DKIslandInFinalBattle}"
# (BG graphics only DMA'd to the SNES VRAM: the 32X draws its own converted tiles / map pictures -
#  WRAM byte-identical without them, sim3 WRAMDUMP over 6 levels + map + a bonus room)
export BLOB_DROP="${BLOB_DROP-SPC700/Samples,SPC700/Music,Graphics/GFX_Layer}"
# files no profile reads (world 1: the Rambi bonus room D2 is built from the snow tileset)
export BLOB_FILES="${BLOB_FILES-LevelData/SpriteData/,LevelData/SnowLevels.bin,Tilemaps/Map32/SnowLevels.bin,Graphics/Compressed/GFX_FGBG_SnowLevels.bin}"
export BLOB_BANKS="${BLOB_BANKS-C0,C1,F5,F6,F8,F9,FB,FC,FD,FE,FF}"
python3 tools/recomp/mkgame.py "$ROM" ${MEMPROF:-tools/recomp/data/memprof.txt} build/game $(for f in $SNAPDIR/*.wai; do b=$(basename $f .wai); echo $f:$b; done)
# the data pack (resident part + 512 KB level banks; snapshots go into their level's bank)
python3 tools/build_viewer.py "$ROM" build/gen2 $LEVELS
GAME_BASE=$(python3 -c "import os; n=os.path.getsize('build/gen2/pack.bin'); print(hex(0x02000000 + ((0x40000 + n + 0xFFF) & ~0xFFF)))")

MDFLAGS="$EXTRA_MDFLAGS -m68000 -O2 -fomit-frame-pointer -ffreestanding -fno-builtin -fno-pic -fno-pie -nostdlib -Imd -Wall"
$MDCC $MDFLAGS -x assembler-with-cpp -c md/boot.s -o build/md_boot.o
$MDCC $MDFLAGS -c md/main.c -o build/md_main.o
m68k-linux-gnu-ld -T md/md.ld build/md_boot.o build/md_main.o -o build/md.elf 2>/dev/null
m68k-linux-gnu-objcopy -O binary build/md.elf build/md.bin

# the music engine: translated from the SPC700 engine bytes in your ROM (tools/meta layout; not shipped)
[ -f sh/spceng_gen.c ] || python3 tools/audio/spc2x.py "$ROM" c sh/spceng_gen.c
SHFLAGS="$EXTRA_SHFLAGS -m2 -mb -O2 -fomit-frame-pointer -ffreestanding -fno-builtin -nostdlib -Wall"
sh-elf-as --small sh/start.s -o build/sh_start.o
python3 tools/gen_comp_asm.py sh/comp.s $(case "$EXTRA_SHFLAGS" in *-DPROF*) echo prof;; esac) $(case "$EXTRA_SHFLAGS" in *-DTCACHE*) echo rom;; esac)
sh-elf-as --small sh/comp.s -o build/sh_comp.o
sh-elf-as --small sh/mixv.s -o build/sh_mixv.o
sh-elf-as --small sh/mixout.s -o build/sh_mixout.o
$SHCC $SHFLAGS -c sh/main.c -o build/sh_main.o
for f in music musplay spcdsp spceng_gen; do $SHCC $SHFLAGS -c sh/$f.c -o build/sh_$f.o; done
GFLAGS="$SHFLAGS -DR65_SH2 -Ish -Itools/recomp -w $GAMEOPT"
for f in game game_hle sprites; do $SHCC $GFLAGS -c sh/$f.c -o build/sh_$f.o; done
for f in r65lib r65interp; do $SHCC $GFLAGS -c tools/recomp/$f.c -o build/sh_$f.o; done
sh-elf-as --small sh/r65tramp.s -o build/sh_r65tramp.o
PROFO=""
case "$EXTRA_SHFLAGS" in *-DPROFH*) sh-elf-as --small sh/prof.s -o build/sh_prof.o; PROFO=build/sh_prof.o;; esac
ls build/g65/game65_*.c | xargs -P 4 -I{} sh -c '$0 $1 -c {} -o build/g65_$(basename {} .c).o' "$SHCC" "$GFLAGS -Ibuild/g65"
# all-worlds ROM (WORLD_PROFILES set): the SNES page table lives at the start of each world bank
# (cart window 6, 0x02300000) - tools/worldbanks.py builds the banks after the link
BANKED=""; PGSYM=""
if [ -n "$WORLD_PROFILES" ]; then BANKED="-DSNES_PG_BANKED"; PGSYM="-Wl,--defsym,_snes_pg=0x02300000"; fi
$SHCC $GFLAGS $BANKED -c build/game/game_tab.c -o build/game_tab.o
printf '\t.section .gtext,"a"\n\t.balign 16\n\t.global _snes_blob\n_snes_blob:\n\t.incbin "build/game/snes_blob.bin"\n' > build/game_data.s
sh-elf-as --small build/game_data.s -o build/game_data.o
SDLMA=""; [ -n "$WORLD_PROFILES" ] && SDLMA="-Wl,--defsym,SDGAME_LMA=0x36F000"
$SHCC $SHFLAGS -T sh/mars.ld -Wl,--defsym,GAME_BASE=$GAME_BASE $PGSYM $SDLMA build/sh_start.o build/sh_main.o build/sh_comp.o \
    build/sh_mixv.o build/sh_mixout.o build/sh_music.o build/sh_musplay.o build/sh_spcdsp.o build/sh_spceng_gen.o \
    build/sh_game.o build/sh_game_hle.o build/sh_sprites.o build/sh_r65lib.o build/sh_r65interp.o build/sh_r65tramp.o $PROFO build/g65_*.o build/game_tab.o build/game_data.o \
    -lgcc -o build/sh.elf
if [ -n "$WORLD_PROFILES" ]; then
    sh-elf-objcopy -O binary -R .gtext -R .sdgame build/sh.elf build/sh.bin
    sh-elf-objcopy -O binary -j .sdgame build/sh.elf build/sdgame.bin
else
    sh-elf-objcopy -O binary -R .gtext build/sh.elf build/sh.bin
fi
sh-elf-objcopy -O binary -j .gtext build/sh.elf build/gtext.bin
if [ -n "$WORLD_PROFILES" ]; then
    python3 tools/worldbanks.py "$ROM" build/game/worlds.json build/sh.elf build/gen2/worldbanks.bin
    python3 tools/finalize.py build/sh.bin build/gen2/pack.bin "$OUT" build/gtext.bin $GAME_BASE build/gen2/banks.bin build/gen2/worldbanks.bin build/sdgame.bin 0x36F000
else
    python3 tools/finalize.py build/sh.bin build/gen2/pack.bin "$OUT" build/gtext.bin $GAME_BASE build/gen2/banks.bin
fi
