#!/usr/bin/env python3
"""Build the Donkey Kong Country 32X port from your own ROM (cross-platform; what build.sh does,
with the all-worlds settings).  No game data ships with the tool: everything below reads your ROM.

  build.py ROM [OUT.32x] [--toolchain DIR] [--snaps DIR] [--jobs N] [--extra-shflags "..."]

ROM may be .sfc / .smc (copier header) / .zip.  The level-start snapshots (--snaps, made by
tools/capture.py from the same ROM) must exist.  Progress lines start with '##' (the GUI shows them).
"""
import argparse, glob, os, shutil, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = sys.executable
EXE = '.exe' if os.name == 'nt' else ''
D = 'tools/recomp/data'
LEVELS = ('16 0C 01 BF 17 E0 D9 2E 07 31 42 E1 A5 A4 D0 43 0D DE E5 24 6D A7 3E 14 CE E2 40 2F 18 22 27 '
          '41 E3 30 12 0A 36 2B E4 68 D2 A6 D3 6C 4C').split()


def step(msg):
    print('## ' + msg, flush=True)


def run(cmd, **kw):
    r = subprocess.run(cmd, cwd=ROOT, **kw)
    if r.returncode:
        raise SystemExit('build step failed (exit %d): %s' % (r.returncode, ' '.join(map(str, cmd))[:300]))


# (I17) the SDRAM game code's image: in world bank 0 (cart window 6 at power-on), not in the first 256 KB
SDGAME_LMA = 0x36F000                      # (window 6 + 0x6F000: up to 68 KB, after the bank's pages)


def tool(tc, name):
    """the toolchain's program, else the one on PATH (Linux: binutils-sh-elf lives in /usr/bin)"""
    p = os.path.join(tc, 'bin', 'sh-elf-' + name + EXE) if tc else ''
    return p if p and os.path.exists(p) else (shutil.which('sh-elf-' + name) or p)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rom')
    ap.add_argument('out', nargs='?')
    ap.add_argument('--toolchain', default=os.environ.get('DKC_TOOLCHAIN', '/opt/sh-elf' if os.name != 'nt' else
                                                          os.path.join(ROOT, 'toolchain', 'sh-elf')))
    ap.add_argument('--snaps', default=os.environ.get('SNAPDIR', os.path.join(ROOT, 'build', 'snaps')))
    ap.add_argument('--jobs', type=int, default=max(1, os.cpu_count() or 1))
    ap.add_argument('--extra-shflags', default='')
    ap.add_argument('--md-bin', default=os.path.join(ROOT, 'tools', 'prebuilt', 'md.bin'))
    a = ap.parse_args()
    out = os.path.abspath(a.out or os.path.splitext(a.rom)[0] + ' (32X).32x')
    os.chdir(ROOT)
    for d in ('build/gen2', 'build/g65', 'build/game', 'build/obj'):
        os.makedirs(d, exist_ok=True)
    rom = 'build/rom.sfc'

    step('Checking the ROM')
    run([PY, 'tools/romcheck.py', os.path.abspath(a.rom), rom])

    step('Recompiling the game code')
    run([PY, 'tools/recomp/r65.py', rom, D, 'build/g65', '--translate', D + '/translate.txt', '--files', '8',
         '--sdram', D + '/sdram.txt', '--hot', D + '/hot.txt', '--prof', D + '/pcprof.txt', '--lowrom', D + '/lowrom.txt'])

    step('Collecting the game data pages')
    env = os.environ
    env['WORLD_PROFILES'] = ','.join('%d:%s/%s:%s/dma_w%d.txt' % (k, D, 'memprof.txt' if k == 1 else 'memprof_w%d.txt' % k, D, k)
                                     for k in range(1, 8))
    env['WORLD_RESIDENT'] = D + '/memprof_rooms.txt,' + D + '/memprof_maps.txt,' + D + '/memprof_fe.txt'
    env['WORLD_CHARS'] = D + '/world_chars.json'
    env['SPR_EXCLUDE'] = ''
    env['BLOB_DROP'] = 'SPC700/Samples,SPC700/Music,Graphics/GFX_Layer'
    env['BLOB_FILES'] = ('LevelData/SpriteData/,1:Tilemaps/Map32/UnderwaterLevels.bin,3:Tilemaps/Map32/UnderwaterLevels.bin,'
                         '4:Tilemaps/Map32/UnderwaterLevels.bin,1:LevelData/SnowLevels.bin,1:Tilemaps/Map32/SnowLevels.bin,'
                         '1:Graphics/Compressed/GFX_FGBG_SnowLevels.bin,Graphics/GFX_Sprite_AreaNameFont.bin,'
                         '6:LevelData/JungleLevels.bin,2:Graphics/DynamicSprites/GFX_QueenB_Fly1.bin,'
                         '5:Graphics/DynamicSprites/GFX_QueenB_Fly1.bin,6:Graphics/DynamicSprites/GFX_QueenB_Fly1.bin')
    env['BLOB_BANKS'] = 'C0,C1,F5,F6,F8,F9,FB,FC,FD,FE,FF'
    snaps = sorted(glob.glob(os.path.join(a.snaps, '*.wai')))
    if not snaps:
        raise SystemExit('no level snapshots in %s (run tools/capture.py first)' % a.snaps)
    run([PY, 'tools/recomp/mkgame.py', rom, D + '/memprof_all.txt', 'build/game'] +
        ['%s:%s' % (f, os.path.splitext(os.path.basename(f))[0]) for f in snaps])

    step('Converting levels, maps and music')
    run([PY, 'tools/build_viewer.py', rom, 'build/gen2'] + LEVELS)
    # (I11: 256-byte aligned - the 4 KB rounding wasted up to 4 KB of the resident 3 MB)
    game_base = 0x02000000 + ((0x40000 + os.path.getsize('build/gen2/pack.bin') + 0xFF) & ~0xFF)

    # the 68000 side (no game data in it): shipped prebuilt; rebuilt when a 68000 compiler is around
    if shutil.which('m68k-linux-gnu-gcc') and not os.environ.get('DKC_PREBUILT_MD'):
        mf = ['-m68000', '-O2', '-fomit-frame-pointer', '-ffreestanding', '-fno-builtin', '-fno-pic', '-fno-pie',
              '-nostdlib', '-Imd', '-Wall'] + os.environ.get('EXTRA_MDFLAGS', '').split()
        run(['m68k-linux-gnu-gcc'] + mf + ['-x', 'assembler-with-cpp', '-c', 'md/boot.s', '-o', 'build/md_boot.o'])
        run(['m68k-linux-gnu-gcc'] + mf + ['-c', 'md/main.c', '-o', 'build/md_main.o'])
        run(['m68k-linux-gnu-ld', '-T', 'md/md.ld', 'build/md_boot.o', 'build/md_main.o', '-o', 'build/md.elf'],
            stderr=subprocess.DEVNULL)
        run(['m68k-linux-gnu-objcopy', '-O', 'binary', 'build/md.elf', 'build/md.bin'])
    else:
        shutil.copy(a.md_bin, 'build/md.bin')

    step('Translating the music engine')
    run([PY, 'tools/audio/spc2x.py', rom, 'c', 'build/spceng_gen.c'], stdout=subprocess.DEVNULL)

    step('Compiling (this is the slow part)')
    gcc, gas = tool(a.toolchain, 'gcc'), tool(a.toolchain, 'as')
    shf = ['-m2', '-mb', '-O2', '-fomit-frame-pointer', '-ffreestanding', '-fno-builtin', '-nostdlib', '-Wall',
           '-DTCACHE'] + a.extra_shflags.split()
    gf = shf + ['-DR65_SH2', '-Ish', '-Itools/recomp', '-w', '-DR65_OOL']
    run([PY, 'tools/gen_comp_asm.py', 'build/comp.s', 'rom'])
    jobs = []
    for s, o in (('sh/start.s', 'sh_start'), ('build/comp.s', 'sh_comp'), ('sh/mixv.s', 'sh_mixv'),
                 ('sh/mixout.s', 'sh_mixout'), ('sh/r65tramp.s', 'sh_r65tramp')):
        jobs.append([gas, '--small', s, '-o', 'build/obj/%s.o' % o])
    for f in ('main', 'music', 'musplay', 'spcdsp'):
        jobs.append([gcc] + shf + ['-c', 'sh/%s.c' % f, '-o', 'build/obj/sh_%s.o' % f])
    # (I19) the translated SPC700 engine at -Os: 28 KB -> 13 KB of resident ROM (it runs a few hundred
    # instructions per engine tick from cold cache lines - the line fills are its cost, not the -O2 code)
    jobs.append([gcc] + [f for f in shf if f != '-O2'] + ['-Os', '-Ish', '-c', 'build/spceng_gen.c', '-o', 'build/obj/sh_spceng_gen.o'])
    for f in ('game', 'game_hle', 'sprites'):
        jobs.append([gcc] + gf + ['-c', 'sh/%s.c' % f, '-o', 'build/obj/sh_%s.o' % f])
    for f in ('r65lib', 'r65interp'):
        jobs.append([gcc] + gf + ['-c', 'tools/recomp/%s.c' % f, '-o', 'build/obj/sh_%s.o' % f])
    g65 = sorted(glob.glob('build/g65/game65_*.c'))
    for f in g65:
        jobs.append([gcc] + gf + ['-Ibuild/g65', '-c', f, '-o', 'build/obj/g65_%s.o' % os.path.splitext(os.path.basename(f))[0]])
    jobs.append([gcc] + gf + ['-DSNES_PG_BANKED', '-c', 'build/game/game_tab.c', '-o', 'build/obj/game_tab.o'])
    with open('build/game_data.s', 'w') as f:
        f.write('\t.section .gtext,"a"\n\t.balign 16\n\t.global _snes_blob\n_snes_blob:\n\t.incbin "build/game/snes_blob.bin"\n')
    jobs.append([gas, '--small', 'build/game_data.s', '-o', 'build/obj/game_data.o'])
    done = [0]

    def one(cmd):
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        done[0] += 1
        if done[0] % 8 == 0:
            print('##p compiled %d of %d' % (done[0], len(jobs)), flush=True)
        return r, cmd
    with ThreadPoolExecutor(a.jobs) as ex:
        for r, cmd in ex.map(one, jobs):
            if r.returncode:
                sys.stderr.write(r.stderr[-2000:])
                raise SystemExit('compile failed: %s' % cmd[-1])

    step('Linking')
    objs = ['build/obj/%s.o' % n for n in ('sh_start', 'sh_main', 'sh_comp', 'sh_mixv', 'sh_mixout', 'sh_music',
                                          'sh_musplay', 'sh_spcdsp', 'sh_spceng_gen', 'sh_game', 'sh_game_hle',
                                          'sh_sprites', 'sh_r65lib', 'sh_r65interp', 'sh_r65tramp')]
    objs += ['build/obj/g65_%s.o' % os.path.splitext(os.path.basename(f))[0] for f in g65]
    objs += ['build/obj/game_tab.o', 'build/obj/game_data.o']
    run([gcc] + shf + ['-T', 'sh/mars.ld', '-Wl,--defsym,GAME_BASE=0x%X' % game_base,
                       '-Wl,--defsym,_snes_pg=0x02300000', '-Wl,--defsym,SDGAME_LMA=0x%X' % SDGAME_LMA]
        + objs + ['-lgcc', '-o', 'build/sh.elf'])
    oc = tool(a.toolchain, 'objcopy')
    run([oc, '-O', 'binary', '-R', '.gtext', '-R', '.sdgame', 'build/sh.elf', 'build/sh.bin'])
    run([oc, '-O', 'binary', '-j', '.gtext', 'build/sh.elf', 'build/gtext.bin'])
    run([oc, '-O', 'binary', '-j', '.sdgame', 'build/sh.elf', 'build/sdgame.bin'])

    step('Assembling the 32X ROM')
    env['DKC_NM'] = tool(a.toolchain, 'nm')
    run([PY, 'tools/worldbanks.py', rom, 'build/game/worlds.json', 'build/sh.elf', 'build/gen2/worldbanks.bin'])
    run([PY, 'tools/finalize.py', 'build/sh.bin', 'build/gen2/pack.bin', out, 'build/gtext.bin', '0x%X' % game_base,
         'build/gen2/banks.bin', 'build/gen2/worldbanks.bin', 'build/sdgame.bin', '0x%X' % SDGAME_LMA])
    print('32X ROM written: ' + out, flush=True)


if __name__ == '__main__':
    main()
