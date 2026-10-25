import json
L = json.load(open('/home/claude/dkc32x/tools/levels.json'))
WORLDS = ['Kongo Jungle','Monkey Mines','Vine Valley','Gorilla Glacier','Kremkroc Industries','Chimp Caverns']
FIRST = ['16','D9','A5','24','40','30']
jobs = []
for lv in L:
    w = lv.get('world'); wi = WORLDS.index(w) if w in WORLDS else (6 if 'Galleon' in lv['name'] else None)
    if wi is None: continue
    e = '%02X' % lv['entrance']
    if not lv.get('room'):
        jobs.append(('%s_pad' % e, e, 3000, {'SCRIPT': '/tmp/claude-0/ver/pad.txt'}))
        jobs.append(('%s_tp' % e, e, 3000, {'TP': '100,90,96'}))
        x0, x1 = lv['x_min'], lv['x_max']; npts = ((x1 - x0) // 96 + 1) * 5
        jobs.append(('%s_grid' % e, e, min(30 + npts * 4 + 10, 8000), {'TPGRID': '%d,%d,96,24,280,64,4' % (x0, x1)}))
    for a in lv.get('aliases', []) + ([lv['entrance']] if lv.get('room') else []):
        if wi < 6:
            jobs.append(('%s_goto_%02X' % (e, a), FIRST[wi], 900, {'GOTO': '40:%02X' % a, 'SCRIPT': '/tmp/claude-0/ver/pad.txt'}))
jobs.append(('fe_menu', '100', 2600, {'JUMP0': '0080BA2C', 'SCRIPT': '/tmp/claude-0/fg/menu.txt'}))
jobs.append(('fe_idle', '100', 4400, {'JUMP0': '0080BA2C'}))
jobs.append(('ending', '68', 5000, {'JUMPF': '30:00B884D7'}))
JOBS = jobs
