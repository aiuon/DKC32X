#!/usr/bin/env python3
"""perf.py ROM ELF OUT.json CONFIG... : MiSTer-model ares runs of a -DPERFREC -DNOHUD build.
CONFIG = level[:pipe[:flags]] (pack level index; pipe 1 = 60 FPS mode; flags 1 = no frame skip, 2 = 60 FPS mode forced, 4 = fused job in the normal mode, 8 = no async swap).
Each run: perf_cfg patched into a ROM copy, ares (ForceInterpreter, ARES_SDFILL=30 ARES_UNCSD=11)
until ARES_QUIT_AT, SDRAM dumped (ARES_SDDUMP), perf_rec decoded."""
import json, os, struct, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor
rom, elf, outf = sys.argv[1], sys.argv[2], sys.argv[3]
cfgs = sys.argv[4:]
QUIT = int(os.environ.get('PERF_QUIT', '2400'))
nm = subprocess.run(['sh-elf-nm', elf], capture_output=True, text=True).stdout
sym = {l.split()[2]: int(l.split()[0], 16) for l in nm.splitlines() if len(l.split()) == 3}
cfg_off = sym['_perf_cfg'] - 0x02000000
rec_off = sym['_perf_rec'] - 0x06000000
data = bytearray(open(rom, 'rb').read())
assert struct.unpack('>I', data[cfg_off:cfg_off + 4])[0] == 0x50434647, 'perf_cfg magic not found'
work = tempfile.mkdtemp(prefix='perf_', dir='/tmp/claude-0')
def run(c):
    p = [int(x) for x in c.split(':')] + [0, 0]
    lv, pipe, fl = p[0], p[1], p[2]
    d = bytearray(data)
    d[cfg_off + 4:cfg_off + 16] = struct.pack('>III', lv, pipe, fl)
    rp = '%s/r_%s.32x' % (work, c.replace(':', '_'))
    sd = rp + '.sd'
    open(rp, 'wb').write(d)
    env = dict(os.environ, DISPLAY=':99', ARES_SDFILL='30', ARES_UNCSD='11', ARES_QUIT_AT=str(QUIT), ARES_SDDUMP=sd)
    subprocess.run(['timeout', '3000', '/opt/ref/ares/build/rundir/bin/ares', '--system', 'Mega 32X', '--no-file-prompt',
                    '--setting', 'Video/Driver=OpenGL 3.2', '--setting', 'Video/Blocking=false', '--setting',
                    'Audio/Blocking=false', '--setting', 'Input/Defocus=Allow', '--setting', 'Developer/ForceInterpreter=true', rp],
                   env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    r = {'cfg': c, 'level': lv, 'pipe': pipe, 'flags': fl}
    try:
        s = open(sd, 'rb').read()
        R = struct.unpack('>64I', s[rec_off:rec_off + 256])
    except Exception as e:
        r['error'] = 'no dump'; return r
    os.remove(rp); os.remove(sd)
    if R[0] != 0x444F4E45:
        r['error'] = 'window not finished (pictures %d)' % R[1]; return r
    pics, vbl, gf = R[1], R[2], R[3]
    r.update(fps=round(pics * 60 / vbl, 1), game_speed=round(gf * 60 / vbl, 1), pictures=pics, vblanks=vbl,
             hist=list(R[4:12]), longest=R[12], pipe_pictures=R[13],
             slave_wait=round(R[14] / R[16] / vbl * 100, 1))
    ft = R[16] * pics
    tot = vbl * R[16]
    ph = [R[19 + i] for i in range(19)]
    r['master_pct_per_pic'] = dict(zip(['logic', 'help', 'wait_slave', 'present', 'spr_prepare', 'to_spr', 'setup_kick', 'iter_end_to_logic', 'kick_to_render_end', 'render_end_to_stage7', 'stage7_to_8', 'spr_master', 'spr_wait', 'spr_slave', 'deferchk', 'kick_localcopy', 'm_logic_end_to_j3', 'm_j3_to_cam', 'm_cam_to_bgsetup'], [round(100 * x / ft, 1) for x in ph]))
    if R[57]: r['pad_presses'] = '%d of %d' % (R[56], R[57])
    r['lost_vblanks'] = dict(after_async=R[60], after_sync=R[61], loop_break=R[62], long_gaps=R[63])
    r['async_pictures'] = R[58]; r['swap_wait_pct_per_pic'] = round(100 * R[59] / ft, 1)
    L = R[40:56]
    if L[0]: r['pipe_lines'] = dict(n=L[0], logic_end=round(L[1] / L[0]), slave_done=round(L[2] / L[0]), done_before_logic=L[3], slave_late=L[4], logic_late=L[5], present_line=round(L[6] / L[0]), present_late=L[7], logic_end_hist32=list(L[8:16]))
    r['master_pct_per_pic']['other'] = round(100 * (tot - sum([ph[i] for i in range(19) if i not in (11, 12, 13)])) / ft, 1)
    return r
res = []
with ThreadPoolExecutor(int(os.environ.get('PERF_J', '2'))) as ex:
    for r in ex.map(run, cfgs):
        print(json.dumps(r), flush=True); res.append(r)
json.dump(res, open(outf, 'w'), indent=1)
