#!/usr/bin/env python3
"""World-bank sweep of every entrance alias (bonus-room exits, checkpoints...): GOTO from the world's
first level, 400 frames each; prints WORLD / MISSING page violations (new ones only)."""
import json, os, subprocess, sys
D = '/home/claude/dkc32x'
L = json.load(open(D + '/tools/levels.json'))
WORLDS = ['Kongo Jungle', 'Monkey Mines', 'Vine Valley', 'Gorilla Glacier', 'Kremkroc Industries', 'Chimp Caverns']
FIRST = ['16', 'D9', 'A5', '24', '40', '30']
seen = {}
jobs = []
for lv in L:
    if lv.get('world') not in WORLDS:
        continue
    wi = WORLDS.index(lv['world'])
    for a in [lv['entrance']] + list(lv.get('aliases', [])):
        jobs.append((wi, '%02X' % a, lv['name']))
if len(sys.argv) > 1:
    jobs = [j for j in jobs if j[1] in sys.argv[1:]]
def run(j):
    wi, e, n = j
    env = dict(os.environ, WORLDBANK=str(wi), GOTO='40:%s' % e)
    r = subprocess.run(['./sim_vs', FIRST[wi], '500'], cwd='/opt/work/sim3', env=env, capture_output=True, text=True)
    return j, [l for l in (r.stdout + '\n' + r.stderr).splitlines() if 'WORLD page' in l or ('MISSING page' in l and ' A00 ' not in l and ' E77 ' not in l)]
from concurrent.futures import ThreadPoolExecutor
with ThreadPoolExecutor(8) as ex:
    for (wi, e, n), ls in ex.map(run, jobs):
        new = []
        for l in ls:
            k = l.split(' key')[0]
            if k not in seen:
                seen[k] = e; new.append(l)
        print('%s %-28s w%d: %d' % (e, n, wi + 1, len(new)), flush=True)
        for l in new: print('    ' + l, flush=True)
