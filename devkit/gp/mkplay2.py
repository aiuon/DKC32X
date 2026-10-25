import random, sys
# gameplay-only trace: enable tracing 180 frames after entering
e = int(sys.argv[1], 16); rnd = random.Random(int(sys.argv[2])); n = int(sys.argv[3])
out = ['load ../sg/wmap.st', 'pokew 0x40 %d' % e, 'pokew 0x3E %d' % e, 'run 6 START', 'run 60', 'unpoke', 'run 120', 'dump x 0x1010 1']
exec(open('/opt/work/gp/mkplay_body.py').read())
