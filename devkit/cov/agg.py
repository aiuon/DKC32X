# agg.py DIR OUT.pkl: interpreted instruction totals per key over a coverage run (xc files), debug-only
# free-movement keys excluded; -> (xg gameplay, xa all, runs key->[jobs])
import glob, collections, pickle, sys
DBG = {0x80A1C0, 0xBCB38E, 0xBCB392, 0xBFC63E}
xg = collections.Counter(); xa = collections.Counter(); runs = {}
for f in glob.glob(sys.argv[1] + '/*.xc'):
    n = f.split('/')[-1][:-3]
    for l in open(f):
        p = l.split(); k = int(p[0], 16)
        if k in DBG or p[2] != '0': continue
        xa[k] += int(p[3]); xg[k] += int(p[4])
        if int(p[4]): runs.setdefault(k, []).append(n)
pickle.dump((xg, xa, runs), open(sys.argv[2], 'wb'))
print('keys', len(xa), 'gameplay', sum(xg.values()), 'all', sum(xa.values()))
for k, v in xg.most_common(30): print('%08X %8d %8d %d' % (k, v, xa[k], len(runs.get(k, []))))
