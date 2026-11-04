import sys
a, b, out = sys.argv[1:4]
A = {}
for ln in open(a):
    q = ln.split(); A[q[0]] = [int(x) for x in q[1:]]
with open(out, 'w') as o:
    for ln in open(b):
        q = ln.split(); v = [int(x) for x in q[1:]]; w = A.get(q[0], [0] * len(v))
        d = [x - y for x, y in zip(v, w)]
        if d[0] > 0: o.write(q[0] + ' ' + ' '.join(str(x) for x in d) + '\n')
