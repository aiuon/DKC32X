#!/usr/bin/env python3
"""One-step conversion: your Donkey Kong Country ROM -> a 32X ROM next to it.

  convert.py ROM [OUT.32x]

Runs tools/romcheck.py, tools/capture.py (emulator captures from the ROM) and tools/build.py with the
programs bundled in this folder (runtime/python, toolchain/sh-elf, emu/).  Work files go to
work/ (deleted when done unless --keep).  Progress lines start with '##' (the GUI reads them)."""
import os, shutil, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = '.exe' if os.name == 'nt' else ''
LIB = '.dll' if os.name == 'nt' else '.so'


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    keep = '--keep' in sys.argv
    if not args:
        print('usage: convert.py ROM [OUT.32x]')
        sys.exit(1)
    rom = os.path.abspath(args[0])
    base = os.path.splitext(rom)[0]
    out = os.path.abspath(args[1]) if len(args) > 1 else base + ' (32X).32x'
    work = os.path.join(ROOT, 'work')
    try:
        os.makedirs(work, exist_ok=True)
        open(os.path.join(work, '.w'), 'w').close()
    except OSError:
        print('The builder folder is not writable (%s).  Unzip it somewhere else, e.g. your Desktop '
              'or Documents, and run it from there.' % ROOT)
        sys.exit(1)
    if not os.path.isfile(rom):
        print('ROM file not found: %s' % rom)
        sys.exit(1)
    t0 = time.time()
    env = dict(os.environ)
    env['DKC_SURVEY'] = os.path.join(work, 'survey')
    env['DKC_HARNESS'] = os.environ.get('DKC_HARNESS', os.path.join(ROOT, 'emu', 'harness' + EXE))
    env['DKC_TMP'] = os.path.join(work, 'tmp')
    tc = os.path.join(ROOT, 'toolchain', 'sh-elf')
    if os.path.isdir(tc):
        env['DKC_TOOLCHAIN'] = tc
    env['DKC_PREBUILT_MD'] = '1'
    os.makedirs(env['DKC_TMP'], exist_ok=True)
    py = sys.executable

    def run(cmd):
        r = subprocess.run(cmd, cwd=ROOT, env=env)
        if r.returncode:
            sys.exit(r.returncode)
    print('## Checking the ROM', flush=True)
    norm = os.path.join(work, 'rom.sfc')
    run([py, os.path.join(ROOT, 'tools', 'romcheck.py'), rom, norm])
    core = os.environ.get('DKC_SNES9X_TRACE', os.path.join(ROOT, 'emu', 'snes9x_libretro' + LIB))
    run([py, os.path.join(ROOT, 'tools', 'capture.py'), norm, work, '--core', core, '--harness', env['DKC_HARNESS']])
    run([py, os.path.join(ROOT, 'tools', 'build.py'), norm, out, '--snaps', os.path.join(work, 'snaps')] +
        (['--toolchain', tc] if os.path.isdir(tc) else []))
    if not keep:
        shutil.rmtree(work, ignore_errors=True)
        shutil.rmtree(os.path.join(ROOT, 'build'), ignore_errors=True)
    print('## Finished in %d s' % (time.time() - t0), flush=True)


if __name__ == '__main__':
    main()
