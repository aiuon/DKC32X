#!/bin/bash
# ikiter.sh KEYS... : add keys to translate.txt, build an IKEYPROF ROM, run PicoDrive to 4800, print interpreted keys
cd /home/claude/dkc32x
for k in "$@"; do grep -q "^$k$" tools/recomp/data/translate.txt || echo $k >> tools/recomp/data/translate.txt; done
sort -u -o tools/recomp/data/translate.txt tools/recomp/data/translate.txt
DKC_SURVEY=work/survey DKC_PREBUILT_MD=1 python3 tools/build.py work/rom.sfc /tmp/claude-0/ikp15.32x --snaps work/snaps --extra-shflags="-DFEDBG -DIKEYPROF" > /tmp/claude-0/ikp15.log 2>&1 || { grep -h "AssertionError" /tmp/claude-0/ikp15.log; tail -2 /tmp/claude-0/ikp15.log; cp /tmp/claude-0/lamp_keep.npz build/lamp.npz; exit 1; }
grep -h "translated\|resident end" /tmp/claude-0/ikp15.log
cp build/sh.elf /tmp/claude-0/ikp15.elf; cp /tmp/claude-0/lamp_keep.npz build/lamp.npz
D=/tmp/claude-0/ikpd4; rm -rf $D; mkdir -p $D; cd $D; printf "run 4800\nshot d\n" > s.txt
PD_SDDUMP=$D/sd PD_SDFRAMES=4800 timeout 1200 /opt/harness/harness /opt/ref/pd_dbg/picodrive_libretro.so /tmp/claude-0/ikp15.32x s.txt . > log 2>&1
python3 - <<'PY'
import glob,struct,subprocess
nm=subprocess.run(['sh-elf-nm','/tmp/claude-0/ikp15.elf'],capture_output=True,text=True).stdout
A={l.split()[2]:int(l.split()[0],16) for l in nm.split('\n') if len(l.split())==3 and l.split()[2].startswith('_ikp')}
def rd(d,a,n):
    o=a-0x06000000; return [struct.unpack('>I',bytes(d[(o+4*i+k)^1] for k in range(4)))[0] for i in range(n)]
for f in sorted(glob.glob('/tmp/claude-0/ikpd4/sd_*.bin')):
    d=open(f,'rb').read()
    k=rd(d,A['_ikp_key'],48); n=rd(d,A['_ikp_n'],48); c=rd(d,A['_ikp_c'],48)
    items=sorted([(n[i],k[i],c[i]) for i in range(48) if k[i]],reverse=True)
    print('keys',len(items),'total insns',sum(x[0] for x in items)); print(' '.join('%08X:%d/%d'%(kk,nn,cc) for nn,kk,cc in items[:8]))
PY
