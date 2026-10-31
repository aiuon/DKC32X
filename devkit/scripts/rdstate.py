# rdstate.py STATE ROM ELF OUT.bin : the game's 128 KB WRAM from a user's ares state (I21h build)
import struct, subprocess, sys
st, romp, elf, out = sys.argv[1:5]
s = open(st, 'rb').read(); rom = open(romp, 'rb').read()
sd = bytearray(s[0x123B:0x123B + 0x40000])
sd = bytes(sd[i ^ 1] for i in range(len(sd)))
nm = subprocess.run(['sh-elf-nm', elf], capture_output=True, text=True).stdout
sym = {l.split()[2]: int(l.split()[0], 16) for l in nm.splitlines() if len(l.split()) == 3}
def rd(a, n):
    a &= 0x1FFFFFFF
    if 0x06000000 <= a < 0x06040000: return sd[a - 0x06000000:a - 0x06000000 + n]
    if 0x02000000 <= a < 0x02400000: return rom[a - 0x02000000:a - 0x02000000 + n]
    return None
w = bytearray(0x20000)
w[0:0x2000] = rd(sym['_wram_lo'], 0x2000)
hp = sym['_wram_hp']
kinds = {}
for p in range(512):
    ptr = struct.unpack('>I', rd(hp + 4 * p, 4))[0]
    if p < 32: continue
    d = rd(ptr, 256)
    kinds[(ptr >> 24) & 0x1F] = kinds.get((ptr >> 24) & 0x1F, 0) + 1
    if d is not None and len(d) == 256: w[p * 256:p * 256 + 256] = d
open(out, 'wb').write(w)
print('page kinds', kinds)
g = rd(sym['_game_stats'], 20); print('game frames', struct.unpack('>5I', g))
