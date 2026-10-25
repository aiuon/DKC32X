t = 0
while t < n:
    r = rnd.random()
    d = rnd.choice([8, 15, 25, 40, 60])
    if r < 0.35: b = 'RIGHT,Y'
    elif r < 0.5: b = 'RIGHT'
    elif r < 0.65: b = 'RIGHT,B' if rnd.random() < .7 else 'RIGHT,Y,B'
    elif r < 0.7: b = 'LEFT'
    elif r < 0.75: b = 'LEFT,Y,B'
    elif r < 0.8: b = 'B'
    elif r < 0.83: b = 'DOWN'
    elif r < 0.86: b = 'SELECT'
    elif r < 0.88: b = 'A'
    elif r < 0.91: b = 'UP'
    elif r < 0.94: b = 'RIGHT,DOWN,Y'
    else: b = ''
    if b in ('SELECT', 'A'): d = 2
    out.append('run %d %s' % (d, b)); t += d
    if rnd.random() < 0.3: out.append('run %d' % rnd.choice([1, 2, 4])); t += 2
out += ['shot end_%02x_%s' % (e, sys.argv[2]), 'dump y 0x1018 8', 'dump z 0x1016 8']
print('\n'.join(out))
