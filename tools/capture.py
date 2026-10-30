#!/usr/bin/env python3
"""Everything the build needs from running YOUR ROM in an emulator (headless snes9x through the
libretro harness), made on your machine - nothing captured ships with the tool:

  1. a start state on the Kongo Jungle map (cold boot + a fixed button script)
  2. per level / bonus room: the VRAM and BG register dumps at its start (DKC_SURVEY layout)
  3. per main level: the level-start snapshot (CPU state, WRAM, VRAM, CGRAM) the 32X game starts from
  4. the hanging-lamp light cone and the snow levels' weather palettes (build/lamp.npz, weather.npz)

  capture.py ROM OUTDIR [--core CORE] [--harness HARNESS] [--jobs N]
Progress lines start with '##'."""
import argparse, json, os, shutil, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = '.exe' if os.name == 'nt' else ''
LIB = '.dll' if os.name == 'nt' else '.so'
# cold boot -> title -> file select -> one player -> the Kongo Jungle map
BOOT = ['run 300', 'run 30 START', 'run 200', 'run 30 START', 'run 200', 'run 30 START', 'run 200',
        'run 30 START', 'run 300', 'run 30 START', 'run 300', 'run 30 START', 'run 300', 'run 30 A',
        'run 30 A', 'run 600', 'state wmap.st']
MAIN = ('16 0C 01 BF 17 E0 D9 2E 07 31 42 E1 A5 A4 D0 43 0D DE E5 24 6D A7 3E 14 CE E2 40 2F 18 22 27 '
        '41 E3 30 12 0A 36 2B E4 68 4C').split()


def step(msg):
    print('## ' + msg, flush=True)


def harness(h, core, rom, script, cwd, env=None):
    with open(os.path.join(cwd, 's.txt'), 'w') as f:
        f.write('\n'.join(script) + '\n')
    e = dict(os.environ)
    e.update(env or {})
    r = subprocess.run([h, core, rom, 's.txt', cwd], cwd=cwd, env=e, stdout=subprocess.DEVNULL,
                       stderr=subprocess.PIPE, text=True)
    if r.returncode:
        raise SystemExit('emulator run failed in %s: %s' % (cwd, r.stderr[-500:]))


def boot_snapshot(h, core, rom, snaps, out):
    """The complete game starts from the power-on state: the game's first WAI in its main wait loop
    (80C0B1, frame ~76, after the boot code's SPC upload) -> snaps/100.wai (key 0x100 = no entrance)."""
    dst = os.path.join(snaps, '100.wai')
    if os.path.exists(dst):
        return
    d = tempfile.mkdtemp(dir=out)
    tmp = os.path.join(d, 'boot.wai')
    harness(h, core, rom, ['run 1', 'dump x 0x1010 1', 'run 100', 'dump z 0x1016 8'], d,
            env={'TRWAI': 'cb', 'TRWAIFILE': tmp})
    rec = 48 + 0x20000 + 0x2000
    data = open(tmp, 'rb').read()
    for i in range(len(data) // rec):
        pc = int.from_bytes(data[i * rec + 44:i * rec + 48], 'little')
        if pc == 0x80C0B1:
            open(dst, 'wb').write(data[:(i + 1) * rec])
            break
    shutil.rmtree(d, ignore_errors=True)
    if not os.path.exists(dst):
        raise SystemExit('no power-on snapshot (the game never reached its wait loop 80C0B1)')


FE_SCRIPT = [(1600, ''), (6, 'START'), (250, ''), (6, 'START'), (250, ''), (6, 'B'), (250, '')]
# the GAME OVER screen: from the map state, no lives left (kept negative), into Jungle Hijinxs and right
# until the Kongs have run into enough Gnawties
GO_PRE = ['pokew 0x575 0xFFFF', 'pokew 0x40 0x16', 'pokew 0x3E 0x16', 'run 6 START', 'run 30', 'unpoke',
          'pokew 0x575 0xFFFF']
GO_SCRIPT = [(1800, 'RIGHT')]
# the ending (Cranky's speech after K. Rool): into Gang-Plank Galleon, then the trace core's TRJUMP
# runs the "boss beaten" code (CODE_B884D7) at the next NMI after harness frame 400
END_PRE = ['pokew 0x40 0x68', 'pokew 0x3E 0x68', 'run 6 START', 'run 60', 'unpoke', 'run 300', 'dump x 0x1010 1']
END_SCRIPT = [(900, '')]
END_ENV = {'TRJUMP': '400:00B884D7'}
# the credits: a main-loop "level" without a level (entrance 5E: DK's treehouse, the cast as sprites)
# -> keyed by the entrance (fe.json 'ent'), drawn by fescr.c with Gang-Plank Galleon's world bank
CRED_PRE = ['pokew 0x40 0x5E', 'pokew 0x3E 0x5E', 'run 6 START', 'run 60', 'unpoke']
CRED_SCRIPT = [(200, '')]
# the full banana hoard after the ending (entrance 4C, one screen): its bananas are BG2 tiles in front
# of BG1's cave - the level renderer has BG2 behind the 32X layer, fescr.c draws the priorities
HOARD_PRE = ['pokew 0x40 0x4C', 'pokew 0x3E 0x4C', 'run 6 START', 'run 60', 'unpoke']
ENT_WORLD = {0x5E: 'Gang-Plank Galleon', 0x4C: 'Kongo Jungle', 0x47: 'Kongo Jungle', 0x5C: 'Kongo Jungle'}


def ent_pre(e):
    return ['pokew 0x40 0x%02X' % e, 'pokew 0x3E 0x%02X' % e, 'run 6 START', 'run 60', 'unpoke']


def fe_capture(h, core, rom, out, wmap):
    """The front-end screens (Rare / Nintendo logos, Cranky's intro, title, (c), file select, player
    select, GAME OVER): fixed button scripts, a dump every 10 frames; per screen (its NMI routine $1C
    and BG mode) the VRAM of the first dump with the screen fully on -> survey/fe/NNNN_M.vram + fe.json.
    (All of a screen's VRAM is written in its setup frame; the 32X keeps the maps up to date itself.)"""
    dst = os.path.join(out, 'survey', 'fe')
    if os.path.exists(os.path.join(dst, 'fe.json')):
        return
    os.makedirs(dst, exist_ok=True)
    seen = {}
    runs = ((None, [], FE_SCRIPT, None, None), (wmap, GO_PRE, GO_SCRIPT, None, {0xDEF9}),
            (wmap, END_PRE, END_SCRIPT, END_ENV, {0xEEA3}), (wmap, CRED_PRE, CRED_SCRIPT, None, 0x5E),
            (wmap, HOARD_PRE, CRED_SCRIPT, None, 0x4C),
            # the empty hoard (from Jungle Hijinxs) and DK's treehouse inside (H12): one-screen rooms
            (wmap, ent_pre(0x47), CRED_SCRIPT, None, 0x47), (wmap, ent_pre(0x5C), CRED_SCRIPT, None, 0x5C),
            (None, [], [(4400, '')], None, {0xDAE5}))     # (the title screen of the attract loop's 2nd round)
    for sn, (state, pre, steps_in, env, only) in enumerate(runs):
        ent = only if isinstance(only, int) else None
        d = tempfile.mkdtemp(dir=out)
        head = []
        if state:
            shutil.copy(state, os.path.join(d, 'start.st'))
            head = ['load start.st']
        head += pre
        script, f, steps = list(head), 0, []
        for n, b in steps_in:
            while n > 0:
                k = min(n, 10 - f % 10)
                script.append('run %d %s' % (k, b))
                f += k
                n -= k
                if f % 10 == 0:
                    script += ['dump %d.wram 2 256' % f, 'dump %d.regs 0x1001 0x4400' % f]
                    steps.append(f)
        harness(h, core, rom, script, d, env)
        mine = {}
        for f in steps:
            w = open(os.path.join(d, '%d.wram' % f), 'rb').read()
            r = open(os.path.join(d, '%d.regs' % f), 'rb').read()
            key = (w[0x1C] | w[0x1D] << 8, r[0x2105] & 7)
            if ent is not None:
                if w[0x3E] != ent:
                    continue
                key = (ent, 0x80 | key[1])  # (by the entrance)
            elif only and key[0] not in only:
                continue                    # (the other runs: only their screen)
            if r[0x2100] == 0x0F and key not in seen and key not in mine:
                mine[key] = f
        # second run: the VRAM of the chosen frames
        script, f = list(head), 0
        want = sorted(set(mine.values()))
        for n, b in steps_in:
            while n > 0:
                nxt = min([x for x in want if x > f] + [f + n])
                k = min(n, nxt - f)
                script.append('run %d %s' % (k, b))
                f += k
                n -= k
                if f in want:
                    script += ['dump %d.vram 3 0' % f, 'dump %d.cgram 0x1000 512' % f]
        if want:
            harness(h, core, rom, script, d, env)
        for (nmi, mode), f in sorted(mine.items()):
            name = '%04X_%d' % (nmi, mode) if mode < 0x80 else '%04X_E' % nmi
            for ext in ('vram', 'cgram', 'regs'):
                shutil.copy(os.path.join(d, '%d.%s' % (f, ext)), os.path.join(dst, name + '.' + ext))
            seen[(nmi, mode)] = dict(nmi=nmi, mode=mode, frame=f, name=name, run=sn)
            if mode >= 0x80:
                seen[(nmi, mode)].update(ent=nmi, world=ENT_WORLD[nmi])
        shutil.rmtree(d, ignore_errors=True)
    json.dump([seen[k] for k in sorted(seen)], open(os.path.join(dst, 'fe.json'), 'w'), indent=1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rom')
    ap.add_argument('out')
    ap.add_argument('--core', default=os.environ.get('DKC_SNES9X_TRACE', os.path.join(ROOT, 'emu', 'snes9x_libretro' + LIB)))
    ap.add_argument('--harness', default=os.environ.get('DKC_HARNESS', os.path.join(ROOT, 'emu', 'harness' + EXE)))
    ap.add_argument('--jobs', type=int, default=max(1, os.cpu_count() or 1))
    a = ap.parse_args()
    rom, out = os.path.abspath(a.rom), os.path.abspath(a.out)
    core, h = os.path.abspath(a.core), os.path.abspath(a.harness)
    survey, snaps = os.path.join(out, 'survey'), os.path.join(out, 'snaps')
    os.makedirs(survey, exist_ok=True)
    os.makedirs(snaps, exist_ok=True)

    step('Starting the game in the emulator (title -> map)')
    wmap = os.path.join(out, 'wmap.st')
    d = tempfile.mkdtemp(dir=out)
    harness(h, core, rom, BOOT, d)
    shutil.move(os.path.join(d, 'wmap.st'), wmap)
    shutil.rmtree(d, ignore_errors=True)

    ents = sorted(set('%02X' % lv['entrance'] for lv in json.load(open(os.path.join(ROOT, 'tools', 'levels.json')))))
    step('Capturing %d levels and bonus rooms' % len(ents))

    # (I14) bonus rooms the world map cannot start (the claptrap rooms): their level first, then the
    # game's cheat-code entrance loader (CODE_80806F) through the NMI pointer $1C for one frame
    # (I21c) 05 = Manic Mincers' Klaptrap room (started from the map, the game went back to Jungle Hijinxs:
    # its picture was the wrong level's, the 32X showed the sprites on black)
    JUMP_ROOMS = {'53': '43', 'C3': '14', 'B8': 'A5', '05': '12'}

    def one_survey(e):
        d = os.path.join(survey, e)
        if os.path.exists(os.path.join(d, 'cgram.bin')) and (e not in JUMP_ROOMS or os.path.exists(os.path.join(d, 'jump.ok'))):
            return                          # ((I21c) a jump room captured by an older builder the map way: again)
        os.makedirs(d, exist_ok=True)
        shutil.copy(wmap, os.path.join(d, 'wmap.st'))
        dumps = ['dump vram.bin 3 0', 'dump bg.bin 0x1003 52', 'dump cgram.bin 0x1000 512']
        if e in JUMP_ROOMS:
            p = JUMP_ROOMS[e]
            harness(h, core, rom, ['load wmap.st', 'pokew 0x40 0x%s' % p, 'pokew 0x3E 0x%s' % p, 'run 6 START',
                                   'run 60', 'unpoke', 'run 400'] +
                    ['pokew 0x%X 0x%s' % (a, e) for a in (0x3E, 0x40, 0x2E, 0x565)] +
                    ['pokew 0x1C 0x806F', 'run 1', 'unpoke', 'run 160'] + dumps, d)
            open(os.path.join(d, 'jump.ok'), 'w').close()
        else:
            harness(h, core, rom, ['load wmap.st', 'pokew 0x40 0x%s' % e, 'pokew 0x3E 0x%s' % e, 'run 6 START',
                                   'run 60', 'unpoke', 'run 300'] + dumps, d)
        os.remove(os.path.join(d, 'wmap.st'))

    def one_snap(e):
        if os.path.exists(os.path.join(snaps, e + '.wai')):
            return
        d = tempfile.mkdtemp(dir=out)
        shutil.copy(wmap, os.path.join(d, 'wmap.st'))
        os.makedirs(os.path.join(d, 'snaps'))
        harness(h, core, rom, ['load wmap.st', 'pokew 0x40 %d' % int(e, 16), 'pokew 0x3E %d' % int(e, 16),
                               'run 6 START', 'run 60', 'unpoke', 'run 150', 'dump x 0x1010 1', 'run 3',
                               'dump z 0x1016 8', 'dump snaps/%s.vram 3 65536' % e, 'dump snaps/%s.cgram 4096 512' % e,
                               'dump snaps/%s.fill 4097 32768' % e], d,
                env={'TRWAI': 'cb', 'TRWAIFILE': os.path.join(snaps, e + '.wai')})
        for ext in ('vram', 'cgram', 'fill'):
            p = os.path.join(d, 'snaps', '%s.%s' % (e, ext))
            if os.path.exists(p):
                shutil.move(p, os.path.join(snaps, '%s.%s' % (e, ext)))
        shutil.rmtree(d, ignore_errors=True)
        if not os.path.exists(os.path.join(snaps, e + '.wai')):
            raise SystemExit('no level-start snapshot for entrance %s' % e)

    n = [0]

    def prog(f, items, label):
        def g(x):
            f(x)
            n[0] += 1
            if n[0] % 10 == 0:
                print('##p %s %d of %d' % (label, n[0], len(items)), flush=True)
        return g
    with ThreadPoolExecutor(a.jobs) as ex:
        list(ex.map(prog(one_survey, ents, 'captured'), ents))
    step('Capturing the level-start states of %d levels' % len(MAIN))
    n[0] = 0
    with ThreadPoolExecutor(a.jobs) as ex:
        list(ex.map(prog(one_snap, MAIN, 'captured'), MAIN))

    step('Capturing the power-on state')
    boot_snapshot(h, core, rom, snaps, out)
    step('Capturing the title screens')
    fe_capture(h, core, rom, out, wmap)
    step('Capturing the lamp light and the snow weather')
    env = dict(os.environ, DKC_HARNESS=h)
    os.makedirs(os.path.join(ROOT, 'build'), exist_ok=True)
    for script, name in (('lamp_capture.py', 'lamp.npz'), ('weather_capture.py', 'weather.npz')):
        r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'capture', script), rom, core, wmap,
                            os.path.join(ROOT, 'build', name)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                           text=True)
        if r.returncode:
            raise SystemExit('%s failed: %s' % (script, r.stderr[-500:]))
    step('Captures done')


if __name__ == '__main__':
    main()
