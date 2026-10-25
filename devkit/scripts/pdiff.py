import sys, bisect, subprocess, collections
a, b, elf = sys.argv[1], sys.argv[2], sys.argv[3]
out = subprocess.run(['sh-elf-nm', '-n', elf], capture_output=True, text=True).stdout
syms = []
for ln in out.split('\n'):
    p = ln.split()
    if len(p) == 3 and p[1] in 'tT': syms.append((int(p[0], 16), p[2]))
syms.sort(); addrs = [x for x, _ in syms]
def load(f):
    d = {}
    if f == '-': return d
    for ln in open(f):
        q = ln.split(); d[int(q[0], 16)] = (int(q[1]), int(q[2]))
    return d
A, B = load(a), load(b)
agg = collections.Counter(); tot = 0
for k, (c, n) in B.items():
    c0 = A.get(k, (0, 0))[0]; d = c - c0
    if d <= 0: continue
    i = bisect.bisect_right(addrs, k) - 1
    agg[syms[i][1] if i >= 0 else '?'] += d; tot += d
print('window cycles', tot, '= %.1f frames of 23.01MHz/60' % (tot / 383500))
for n, c in agg.most_common(int(sys.argv[4]) if len(sys.argv) > 4 else 25):
    print('%6.2f%% %s' % (100 * c / tot, n))
