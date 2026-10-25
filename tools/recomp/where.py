import os as _os, sys as _sys
_sys.path.insert(0, _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), '..'))
import paths
import sys, os, pickle
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import disparse
cache = os.path.join(paths.TMP, 'disparse.pkl')
if os.path.exists(cache): insns = pickle.load(open(cache, 'rb'))
else:
    insns = disparse.parse()[0]; os.makedirs(os.path.dirname(cache), exist_ok=True); pickle.dump(insns, open(cache, 'wb'))
a = int(sys.argv[1], 16); before = int(sys.argv[2]) if len(sys.argv) > 2 else 15; after = int(sys.argv[3]) if len(sys.argv) > 3 else 30
k = max((x for x in insns if x <= a and x >> 16 == a >> 16), default=None)
no = insns[k][2]
lines = open(disparse.DIS, encoding='latin-1').read().split('\n')
for n in range(max(0, no - 1 - before), no - 1 + after):
    print('%6d%s %s' % (n + 1, '>' if n + 1 == no else ' ', lines[n]))
