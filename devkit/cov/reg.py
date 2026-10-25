# reg.py REF NEW OUTDIR: every coverage job (all levels x pad / teleport / grid, every room alias,
# front-end, ending) on both sims; per-frame WRAM hashes must match
import json, subprocess, os, sys
from concurrent.futures import ThreadPoolExecutor
REF, NEW, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(OUT, exist_ok=True)
exec(open('/tmp/claude-0/cov/jobs.py').read())
def one(sim, name, e, n, env, tag):
    env = dict(os.environ, **env, WRAMHASH='%s/%s.%s' % (OUT, name, tag))
    try:
        subprocess.run([sim, e, str(n)], env=env, capture_output=True, text=True, cwd=OUT, timeout=1800)
    except subprocess.TimeoutExpired:
        pass
    return '%s/%s.%s' % (OUT, name, tag)
def run(j):
    name, e, n, env = j
    a = one(REF, name, e, n, env, 'a'); b = one(NEW, name, e, n, env, 'b')
    la = open(a).read().splitlines() if os.path.exists(a) else []
    lb = open(b).read().splitlines() if os.path.exists(b) else []
    for x, y in zip(la, lb):
        if x != y: return name, 'DIFF at frame %s' % x.split()[0]
    if len(la) != len(lb) or not la: return name, 'LENGTH %d vs %d' % (len(la), len(lb))
    os.remove(a); os.remove(b)
    return name, 'ok %d' % len(la)
with ThreadPoolExecutor(2) as ex:
    for name, r in ex.map(run, JOBS):
        print(name, r, flush=True)
