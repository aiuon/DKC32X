#!/usr/bin/env python3
"""pic_eq.py ROM ELF LEVEL CFG_A CFG_B [FROM TO]: picture equality of two modes (perf_cfg pipe:flags,
e.g. 0:1 vs 1:3).  With frame skip off every game frame is one picture, so both runs must show the
same sequence of distinct pictures (the 60 FPS mode one frame later).  ares (fast mode) dumps every
frame FROM..TO; consecutive duplicates removed; sequences aligned on the first match."""
import hashlib, os, struct, subprocess, sys, tempfile
rom, elf, lv = sys.argv[1], sys.argv[2], int(sys.argv[3])
ca, cb = sys.argv[4], sys.argv[5]
F0 = int(sys.argv[6]) if len(sys.argv) > 6 else 300
F1 = int(sys.argv[7]) if len(sys.argv) > 7 else 900
nm = subprocess.run(['sh-elf-nm', elf], capture_output=True, text=True).stdout
off = [int(l.split()[0], 16) for l in nm.splitlines() if l.endswith(' _perf_cfg')][0] - 0x02000000
data = bytearray(open(rom, 'rb').read())
work = tempfile.mkdtemp(prefix='piceq_', dir='/tmp/claude-0')
def run(cfg, tag):
    p, f = [int(x) for x in cfg.split(':')]
    d = bytearray(data); d[off + 4:off + 16] = struct.pack('>III', lv, p, f)
    rp = '%s/%s.32x' % (work, tag); open(rp, 'wb').write(d)
    dd = '%s/%s' % (work, tag); os.makedirs(dd)
    env = dict(os.environ, DISPLAY=':99', ARES_DUMP=dd, ARES_DUMP_FROM=str(F0), ARES_DUMP_TO=str(F1), ARES_QUIT_AT=str(F1 + 2))
    if os.environ.get('MISTER'):
        env.update(ARES_SDFILL='30', ARES_UNCSD='11')
    args = ['timeout', '3000', '/opt/ref/ares/build/rundir/bin/ares', '--system', 'Mega 32X', '--no-file-prompt',
            '--setting', 'Video/Driver=OpenGL 3.2', '--setting', 'Video/Blocking=false', '--setting', 'Audio/Blocking=false', '--setting', 'Input/Defocus=Allow']
    if os.environ.get('MISTER'): args += ['--setting', 'Developer/ForceInterpreter=true']
    subprocess.run(args + [rp], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    seq = []; files = []
    for fn in sorted(os.listdir(dd)):
        h = hashlib.md5(open(os.path.join(dd, fn), 'rb').read()).hexdigest()
        if not seq or seq[-1] != h: seq.append(h); files.append(os.path.join(dd, fn))
    return seq, files
from concurrent.futures import ThreadPoolExecutor
with ThreadPoolExecutor(2) as ex:
    (sa, fa), (sb, fb) = ex.map(lambda t: run(*t), [(ca, 'a'), (cb, 'b')])
# align: first picture of b that appears in a
ia = {h: i for i, h in reversed(list(enumerate(sa)))}
best = None
for j in range(min(30, len(sb))):
    if sb[j] in ia: best = (ia[sb[j]], j); break
if not best:
    print('NO COMMON PICTURE (a %d, b %d distinct)' % (len(sa), len(sb))); sys.exit(1)
i, j = best; n = 0; bad = []
while i < len(sa) and j < len(sb):
    if sa[i] != sb[j]: bad.append((fa[i], fb[j]))
    i += 1; j += 1; n += 1
print('%s vs %s level %d: %d pictures compared (a %d, b %d distinct), %d differ' % (ca, cb, lv, n, len(sa), len(sb), len(bad)))
for x, y in bad[:5]: print('  differ:', x, y)
print('work', work)
