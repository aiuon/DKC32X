import json, subprocess, os, sys
from concurrent.futures import ThreadPoolExecutor
SIM = sys.argv[1] if len(sys.argv) > 1 else '/opt/work/sim3/sim_fc'
OUT = sys.argv[2] if len(sys.argv) > 2 else '/tmp/claude-0/cov/r'
os.makedirs(OUT, exist_ok=True)
L = json.load(open('/home/claude/dkc32x/tools/levels.json'))
WORLDS = ['Kongo Jungle','Monkey Mines','Vine Valley','Gorilla Glacier','Kremkroc Industries','Chimp Caverns']
FIRST = ['16','D9','A5','24','40','30']
jobs = []
for lv in L:
    w = lv.get('world'); wi = WORLDS.index(w) if w in WORLDS else (6 if 'Galleon' in lv['name'] else None)
    if wi is None: continue
    e = '%02X' % lv['entrance']
    if not lv.get('room'):
        jobs.append(('%s_pad' % e, e, 3000, {'SCRIPT': '/tmp/claude-0/ver/pad.txt'}))
        jobs.append(('%s_tp' % e, e, 3000, {'TP': '100,90,96'}))
        x0, x1 = lv['x_min'], lv['x_max']; npts = ((x1 - x0) // 96 + 1) * 5
        jobs.append(('%s_grid' % e, e, min(30 + npts * 4 + 10, 8000), {'TPGRID': '%d,%d,96,24,280,64,4' % (x0, x1)}))
    for a in lv.get('aliases', []) + ([lv['entrance']] if lv.get('room') else []):
        if wi < 6:
            jobs.append(('%s_goto_%02X' % (e, a), FIRST[wi], 900, {'GOTO': '40:%02X' % a, 'SCRIPT': '/tmp/claude-0/ver/pad.txt'}))
jobs.append(('fe_menu', '100', 2600, {'JUMP0': '0080BA2C', 'SCRIPT': '/tmp/claude-0/fg/menu.txt'}))
jobs.append(('fe_idle', '100', 4400, {'JUMP0': '0080BA2C'}))
jobs.append(('ending', '68', 5000, {'JUMPF': '30:00B884D7'}))
def run(j):
    name, e, n, env = j
    env = dict(os.environ, **env, FCFILE='%s/%s.fc' % (OUT, name), XCFILE='%s/%s.xc' % (OUT, name), PAGELOG='%s/%s.pg' % (OUT, name))
    try:
        r = subprocess.run([SIM, e, str(n)], env=env, capture_output=True, text=True, cwd=OUT, timeout=1800)
        bad = [l for l in (r.stdout + r.stderr).splitlines() if 'err 2' in l or 'fatal' in l or 'STUCK' in l]
    except subprocess.TimeoutExpired:
        bad = ['TIMEOUT']
    return name, bad
with ThreadPoolExecutor(2) as ex:
    for name, bad in ex.map(run, jobs):
        print(name, 'ok' if not bad else bad[:2], flush=True)
