import random, sys
# gameplay-only trace, no level loads: enable tracing 180 frames after entering, short run
e = int(sys.argv[1], 16); rnd = random.Random(int(sys.argv[2])); n = int(sys.argv[3])
out = ['load ../sg/wmap.st', 'pokew 0x40 %d' % e, 'pokew 0x3E %d' % e, 'run 6 START', 'run 60', 'unpoke', 'run 120', 'dump x 0x1010 1']
t = 0
while t < n:
    r = rnd.random(); d = rnd.choice([8, 15, 25, 40])
    if r < 0.4: b = 'RIGHT,Y'
    elif r < 0.6: b = 'RIGHT'
    elif r < 0.8: b = 'RIGHT,B'
    elif r < 0.85: b = 'LEFT'
    elif r < 0.9: b = 'B'
    else: b = ''
    out.append('run %d %s' % (d, b)); t += d
out += ['dump y 0x1018 8', 'dump z 0x1016 8']
print('\n'.join(out))
