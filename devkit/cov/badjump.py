# list translated functions containing BADJUMP (generated g65 dir)
import re,glob,sys
d=sys.argv[1] if len(sys.argv)>1 else '/home/claude/dkc32x/build/g65'
for f in sorted(glob.glob(d+'/game65_*.c')):
    cur=None
    for l in open(f):
        m=re.match(r'\w*\s*uint32_t (F[0-9A-F]+_\d)\(void\)',l)
        if m: cur=m.group(1)
        if 'BADJUMP(' in l and not l.startswith('#'): print(f.split('/')[-1],cur,l.strip()[:100])
