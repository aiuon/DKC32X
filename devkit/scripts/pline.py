import subprocess,collections,sys
prof,elf,pat=sys.argv[1],sys.argv[2],sys.argv[3]
tot=0; rows=[]
for ln in open(prof):
    q=ln.split(); a=int(q[0],16); c=int(q[1]); tot+=c; rows.append((a,c))
res=subprocess.run(['sh-elf-addr2line','-e',elf]+[hex(a) for a,c in rows],capture_output=True,text=True).stdout.split('\n')
agg=collections.Counter()
for (a,c),r in zip(rows,res):
    r=r.split('/')[-1].split(' ')[0]
    if pat in r: agg[r]+=c
for r,c in agg.most_common(int(sys.argv[4]) if len(sys.argv)>4 else 40): print('%6.2f%% %s'%(100*c/tot,r))
