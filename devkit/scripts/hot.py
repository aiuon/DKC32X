import subprocess, collections, sys
elf, fa, fb, fn = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
out=subprocess.run(['sh-elf-nm','-n',elf],capture_output=True,text=True).stdout
ent=[(int(l.split()[0],16),l.split()[2]) for l in out.split('\n') if len(l.split())==3]
start=[a for a,n in ent if n==fn][0]
end=min(a for a,n in ent if a>start)
def load(f):
    d={}
    if f=='-': return d
    for ln in open(f):
        q=ln.split(); d[int(q[0],16)]=int(q[1])
    return d
A=load(fa); B=load(fb)
h=collections.Counter()
for k,c in B.items():
    if start<=k<end: h[k]=c-A.get(k,0)
tot=sum(h.values())
print(fn, hex(start),hex(end),tot)
b=collections.Counter()
for k,c in h.items(): b[(k-start)//32*32]+=c
for off,c in sorted(b.items(), key=lambda x:-x[1])[:int(sys.argv[5]) if len(sys.argv)>5 else 15]: print(hex(start+off), '%5.1f%%'%(100*c/tot))
