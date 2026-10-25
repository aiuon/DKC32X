import sys, bisect, subprocess, collections
prof, elf = sys.argv[1], sys.argv[2]; N=int(sys.argv[3]) if len(sys.argv)>3 else 40
out = subprocess.run(['sh-elf-nm', '-n', elf], capture_output=True, text=True).stdout
syms = sorted((int(p[0],16), p[2]) for p in (l.split() for l in out.split('\n')) if len(p)==3 and p[1] in 'tT')
addrs=[a for a,_ in syms]
c=collections.Counter(); f=collections.Counter(); ins=collections.Counter(); tot=0
for ln in open(prof):
    q=ln.split(); a=int(q[0],16); i=bisect.bisect_right(addrs,a)-1; n=syms[i][1] if i>=0 else '?'
    c[n]+=int(q[1]); tot+=int(q[1])
    if len(q)>=8: f[n]+=int(q[3])+int(q[4]); ins[n]+=int(q[2])
for n,v in c.most_common(N): print('%-28s %5.2f%%  ins %8d fills %7d  c/i %.1f'%(n,100*v/tot,ins[n],f[n],v/max(ins[n],1)))
