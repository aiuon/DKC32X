import sys, bisect, subprocess, collections
prof, elf = sys.argv[1], sys.argv[2]
out = subprocess.run(['sh-elf-nm', '-n', elf], capture_output=True, text=True).stdout
syms = []
for ln in out.split('\n'):
    p = ln.split()
    if len(p) == 3 and p[1] in 'tT': syms.append((int(p[0], 16), p[2]))
syms.sort(); addrs = [a for a, _ in syms]
agg = collections.Counter(); cnt = collections.Counter(); tot = 0; gt = 0; gn = 0
gstart = [a for a, n in syms if n == '_gtext_start']
gstart = gstart[0] if gstart else 1 << 40
for ln in open(prof):
    q = ln.split(); a = int(q[0], 16); c = int(q[1]); k = int(q[2]) if len(q) > 2 else 0
    i = bisect.bisect_right(addrs, a) - 1
    name = syms[i][1] if i >= 0 else '?'
    agg[name] += c; cnt[name] += k; tot += c
    if a >= gstart and a < 0x03000000: gt += c; gn += k
print('total', tot, 'generated code', gt, '%.1f%%' % (100 * gt / tot), 'cycles/insn %.2f' % (gt / max(gn, 1)))
for n, c in agg.most_common(int(sys.argv[3]) if len(sys.argv) > 3 else 50):
    print('%6.2f%% %6.2f c/i %s' % (100 * c / tot, c / max(cnt[n], 1), n))
