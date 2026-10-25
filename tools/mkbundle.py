#!/usr/bin/env python3
"""Assemble the portable Windows builder folder (no game data in it - checked by tools/scanbundle.py).

  mkbundle.py OUTDIR --python DIR --toolchain DIR --core snes9x_libretro.dll --harness harness.exe --launcher EXE

OUTDIR/
  DKC32X Builder.exe      double-click: window with Browse + Convert; or drop a ROM on it
  Convert (drag ROM here).bat
  README.txt
  app/                    the port's source (sh/, md/, tools/) - compiled on the user's PC
  app/emu/                headless snes9x (libretro) + harness, used to capture from the user's ROM
  app/toolchain/sh-elf/   SH-2 gcc/binutils for Windows
  app/runtime/python/     portable Python 3.12 (+ numpy, Pillow, tkinter)
"""
import argparse, os, shutil, subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKIP_DIRS = {'__pycache__', 'banktest', 'build', 'out'}
# generated from the ROM at build time (spceng_gen.c) or by the build (comp.s), or old drafts
SKIP_FILES = {'spceng_gen.c', 'comp.s', 'main_v1.c', 'main_v22pre.c', 'harness.exe'}
SKIP_EXT = {'.o', '.elf', '.32x', '.sfc', '.smc', '.st', '.pyc'}


def copytree(src, dst):
    for dp, dns, fns in os.walk(src):
        dns[:] = [d for d in dns if d not in SKIP_DIRS]
        rel = os.path.relpath(dp, src)
        os.makedirs(os.path.join(dst, rel), exist_ok=True)
        for f in fns:
            if os.path.splitext(f)[1].lower() in SKIP_EXT or f in SKIP_FILES:
                continue
            shutil.copy2(os.path.join(dp, f), os.path.join(dst, rel, f))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--python', required=True)
    ap.add_argument('--toolchain', required=True)
    ap.add_argument('--core', required=True)
    ap.add_argument('--harness', required=True)
    ap.add_argument('--launcher', required=True)
    a = ap.parse_args()
    if os.path.exists(a.out):
        shutil.rmtree(a.out)
    app = os.path.join(a.out, 'app')
    for d in ('sh', 'md', 'tools'):
        copytree(os.path.join(ROOT, d), os.path.join(app, d))
    for f in ('README.md', 'BUILD.md'):
        if os.path.exists(os.path.join(ROOT, f)):
            shutil.copy2(os.path.join(ROOT, f), app)
    os.makedirs(os.path.join(app, 'emu'))
    shutil.copy2(a.core, os.path.join(app, 'emu', 'snes9x_libretro.dll'))
    shutil.copy2(a.harness, os.path.join(app, 'emu', 'harness.exe'))
    tc = os.path.join(app, 'toolchain', 'sh-elf')
    top = os.path.abspath(a.toolchain)
    shutil.copytree(a.toolchain, tc, symlinks=False,          # (docs / top-level headers: not used)
                    ignore=lambda d, names: [n for n in names if os.path.abspath(d) == top and n in ('share', 'include')])
    strip = shutil.which('x86_64-w64-mingw32-strip')
    if strip:                                  # (no debug info: 100 -> ~45 MB)
        for dp, dns, fns in os.walk(tc):
            for f in fns:
                if f.endswith('.exe'):
                    subprocess.run([strip, os.path.join(dp, f)], check=True)
    shutil.copytree(a.python, os.path.join(app, 'runtime', 'python'), symlinks=False,
                    ignore=shutil.ignore_patterns('__pycache__'))
    shutil.copy2(a.launcher, os.path.join(a.out, 'DKC32X Builder.exe'))
    for name in ('Convert (drag ROM here).bat', 'README.txt'):
        shutil.copy2(os.path.join(ROOT, 'tools', 'win', name), os.path.join(a.out, name))
    shutil.copytree(os.path.join(ROOT, 'tools', 'win', 'licenses'), os.path.join(a.out, 'licenses'))
    shutil.rmtree(os.path.join(app, 'tools', 'win'), ignore_errors=True)
    print('bundle ready:', a.out)


if __name__ == '__main__':
    main()
