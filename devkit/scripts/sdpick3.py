# sdpick3.py budget out size_elf prof:elf ... : SDRAM set by summed (normalized) code fills per byte over profiles
import sys, bisect, subprocess, collections
budget, outf, selfe = int(sys.argv[1]), sys.argv[2], sys.argv[3]
def syms(elf):
    out = subprocess.run(['sh-elf-nm', '-n', '-S', elf], capture_output=True, text=True).stdout
    r = []
    for ln in out.split('\n'):
        p = ln.split()
        if len(p) == 4 and p[2] in 'tT': r.append((int(p[0], 16), int(p[1], 16), p[3]))
    return sorted(r)
tot = collections.Counter(); cyc = collections.Counter()
for pe in sys.argv[4:]:
    prof, elf = pe.split(':')
    ps = syms(elf); addrs = [a for a, _, _ in ps]
    f = collections.Counter(); allc = 0; cc = collections.Counter()
    for ln in open(prof):
        q = ln.split()
        if len(q) < 8: continue
        a = int(q[0], 16); allc += int(q[1]); i = bisect.bisect_right(addrs, a) - 1
        if i >= 0 and ps[i][2].startswith('_F') and a < ps[i][0] + ps[i][1]:
            f[ps[i][2]] += int(q[3]) + int(q[4]); cc[ps[i][2]] += int(q[1])
    for n in f: tot[n] += f[n] / allc * 1e6; cyc[n] += cc[n] / allc * 1e6
size = dict((n, z) for _, z, n in syms(selfe))
order = sorted((n for n in tot if n in size), key=lambda n: -tot[n] / (size[n] + 16))
used = 0; got = 0; T = sum(tot.values()); picked = []
for n in order:
    z = size[n] + 16
    if used + z > budget: continue
    used += z; got += tot[n]; picked.append(n)
with open(outf, 'w') as o:
    for n in picked: o.write('%02X%s\n' % (int(n[-1]), n[2:8]))
print('picked %d functions, %d bytes, %.1f%% of code fills' % (len(picked), used, 100 * got / max(T, 1)))
