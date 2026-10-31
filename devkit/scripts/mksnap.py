# mksnap.py STATE ROM ELF REFSNAP OUT: a sim level snapshot (game_init format) from a user's ares state
import struct, subprocess, sys
sys.path.insert(0, '/home/claude/dkc32x/tools/recomp')
from mkgame import lz_pack
st, romp, elf, ref, out = sys.argv[1:6]
s = open(st, 'rb').read(); rom = open(romp, 'rb').read()
sd = bytearray(s[0x123B:0x123B + 0x40000]); sd = bytes(sd[i ^ 1] for i in range(len(sd)))
nm = subprocess.run(['sh-elf-nm', elf], capture_output=True, text=True).stdout
sym = {l.split()[2]: int(l.split()[0], 16) for l in nm.splitlines() if len(l.split()) == 3}
S = lambda n, k: sd[sym[n] - 0x06000000:sym[n] - 0x06000000 + k]
romw = lambda n: struct.unpack('>I', rom[sym[n] - 0x02000000:sym[n] - 0x02000000 + 4])[0]
npool, nring = romw('_game_npool'), romw('_game_nring')
cpu = struct.unpack('>10I', S('_cpu', 40))
print('cpu a %X x %X y %X s %X d %X db %X p %X pc %08X err %X' % cpu[:9], 'npool', npool, 'nring', nring)
w = S('_wram_lo', 0x2000) + S('_wram_pool', npool * 256) + S('_ring_ent', nring * 256) + S('_sram', 0x800)
z = lz_pack(w)
r = open(ref, 'rb').read(); zl = struct.unpack('>I', r[32:36])[0]; tail = r[(32 + 4 + zl + 3) & ~3:]
o = struct.pack('>8I', *cpu[:8]) + struct.pack('>I', len(z)) + z
o += b'\0' * ((-len(o)) & 3)
open(out, 'wb').write(o + tail)
