#!/usr/bin/env python3
"""Music pack for the 32X player, built from the user's own DKC ROM.

The SNES side uploads to the SPC700 (Routine_Macros_DKC1.asm):
  * CODE_8AB12E: engine (DATA_8AA36A, $0D7E bytes -> ARAM $04E0) and the global sound-effect block
    (DATA_C92D95: u16 count, u16 dest, data)
  * CODE_8AB3F1: global sample list DATA_8AB245 -> directory $3200.., data $3300.., SRCN 0..
  * CODE_8AB40F(music id): the music's sample list DATA_8AB20D[id], continuing directory/data/SRCN;
    then the sample-id -> SRCN table ($7E7000, 255 bytes -> ARAM $04E0)
  * CODE_8AB1C6(music id): the song block DATA_8AB159[id] (u16 count, u16 dest, data)
Each sample (DATA_89FF00[id], 24-bit pointers) is {u16 loop offset, u16 length, BRR data}.

Pack layout (big-endian, see sh/music.c):
  'MUS1' u16 nsongs u16 nsamples u32 off_songs u32 off_samples u8 level_song[256] u8 menu_song[4]
         u8 id_song[32] (DKC music id -> song index, 0xFF: not in this pack; the game mode follows the
         game's own song requests: WRAM $0523 / $051D)
  song   (96 bytes): u32 off_tab (u16 chunk index per 64 bytes of ARAM 0..IMG_HI) u32 off_pool (the
         shared 64-byte chunk pool) u16 smp[40] (SRCN -> global sample index, 0xFFFF none)
  sample (12 bytes): u32 off_pcm u32 len u32 loop (== len: no loop); pcm = s16 BE x (len + 1), 15-bit range
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from brr import decode_brr, prefilter  # noqa: E402

ENGINE, ENGINE_LEN, ENGINE_DST = 0x8AA36A, 0x0D7E, 0x04E0
SFX_BLOCK = 0xC92D95
GLOBAL_LIST = 0x8AB245
SET_LISTS = 0x8AB20D
SONG_PTRS = 0x8AB159
SAMPLE_PTRS = 0x89FF00
DIR0, DATA0 = 0x3200, 0x3300
IMG_LO, IMG_HI = 0x0600, 0x3200
NSRC = 40

# music IDs (Misc_Defines_DKC1.asm)
MUSIC = dict(DKIslandSwing=0x00, CaveDweller=0x01, MistyMenace=0x02, AquaticAmbience=0x03,
             MineCartMadness=0x04, NorthernHemispheres=0x05, VoicesOfTheTemple=0x06, FearFactory=0x07,
             LifeInTheMines=0x08, IceCaveChant=0x0B, SimianSegue=0x0C, ForestFrenzy=0x0D,
             TreetopRock=0x13, BadBossBoogie=0x15, GangplankGalleon=0x18)
# level entrance -> music (read from WRAM $0523 200 frames into each level, snes9x)
LEVEL_MUSIC = {
    'DKIslandSwing': [0x0C, 0x0D, 0x16, 0x17], 'CaveDweller': [0x01, 0x07, 0x12, 0x14, 0x18],
    'MistyMenace': [0x0A, 0x31, 0x36], 'AquaticAmbience': [0x22, 0x3E, 0xBF, 0xDE],
    'NorthernHemispheres': [0x24, 0xA7], 'MineCartMadness': [0x27, 0x2E],
    'LifeInTheMines': [0x2B, 0x2F, 0x30, 0xD9], 'FearFactory': [0x40, 0x41],
    'VoicesOfTheTemple': [0x42, 0x43], 'GangplankGalleon': [0x68], 'IceCaveChant': [0x6D],
    'TreetopRock': [0xA4, 0xCE], 'ForestFrenzy': [0xA5, 0xD0],
    'BadBossBoogie': [0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5],
    'SimianSegue': [0xE6, 0xE7, 0xE8, 0xE9, 0xEC, 0xED]}
MENU_SONG = 'SimianSegue'
# songs with no level of their own (jingles, bonus rooms, cabins, game over); MUSIC_EXTRA (env, comma
# list of names) overrides the default set
EXTRA = dict(BonusRoomBlitz=0x10, DeathMusic=0x11, Victory=0x12, BeatLevel=0x1A, Failure=0x19,
             GameOver=0x0F, CrankysTheme=0x17, CandysLoveSong=0x16, FunkysFugue=0x14, TitleTheme=0x09,
             SplashScreenFanfare=0x0A, CreditsConcerto=0x0E)
EXTRA_OFF = ()     # (all reached in the complete game: logos, intro / title, credits)


def word_list(rom, addr):
    out = []
    while True:
        w = rom.u16(addr)
        if w == 0xFFFF:
            return out
        out.append(w)
        addr += 2


def put_block(rom, aram, addr):
    n, dst = rom.u16(addr), rom.u16(addr + 2)
    aram[dst:dst + n] = rom.read(addr + 4, n)
    return dst, n


class Uploader:
    def __init__(self, rom):
        self.rom = rom
        self.aram = bytearray(0x10000)
        self.map = bytearray(0x100)
        self.sample_of_srcn = {}
        self.dir, self.data, self.srcn = DIR0, DATA0, 0

    def samples(self, ids):
        rom, a = self.rom, self.aram
        for sid in ids:
            self.map[sid & 0xFF] = self.srcn
            p = rom.u24(SAMPLE_PTRS + sid * 3)
            loop, n = rom.u16(p), rom.u16(p + 2)
            a[self.data:self.data + n] = rom.read(p + 4, n)
            start, lp = self.data, self.data + loop
            a[self.dir:self.dir + 4] = struct.pack('<HH', start & 0xFFFF, lp & 0xFFFF)
            self.sample_of_srcn[self.srcn] = sid
            self.srcn += 1
            self.dir += 4
            self.data += n


# The sample-id -> SRCN table keeps stale entries from earlier uploads; songs do look some of them up.
# Reproduce the history of a level start (boot leftover, title screen set, world map set) - matches
# snes9x ARAM snapshots taken in every level.
BOOT_MAP = {0x10: 2}
HISTORY = [0x09, 0x0C]


def build_aram(rom, music_id):
    up = Uploader(rom)
    a = up.aram
    a[ENGINE_DST:ENGINE_DST + ENGINE_LEN] = rom.read(ENGINE, ENGINE_LEN)
    put_block(rom, a, SFX_BLOCK)
    for k, v in BOOT_MAP.items():
        up.map[k] = v
    up.samples(word_list(rom, GLOBAL_LIST))
    base = (up.dir, up.data, up.srcn)
    for mid in HISTORY + [music_id]:
        up.dir, up.data, up.srcn = base
        if mid == music_id:
            up.sample_of_srcn = {k: v for k, v in up.sample_of_srcn.items() if k < base[2]}
        lst = rom.u16(SET_LISTS + mid * 2) | (SET_LISTS & 0xFF0000)
        up.samples(word_list(rom, lst))
    a[0x04E0:0x04E0 + 0xFF] = up.map[:0xFF]
    song = rom.u24(SONG_PTRS + music_id * 3)
    dst, n = put_block(rom, a, song)
    return a, up.sample_of_srcn, (dst, n)


def sample_pcm(aram, srcn):
    e = DIR0 + srcn * 4
    start, loop = aram[e] | aram[e + 1] << 8, aram[e + 2] | aram[e + 3] << 8
    pcm16, li = decode_brr(aram, start, loop)
    pcm = [x >> 1 for x in prefilter(pcm16, li)]     # 15-bit: the SH-2 mixer uses muls.w
    loop_i = li if li is not None else len(pcm)
    pad = pcm[li] if li is not None else 0
    return pcm + [pad], len(pcm), loop_i


def build_music(rom, E='>'):
    """-> bytes of the music block (offsets relative to its start); E = struct byte order"""
    want = os.environ.get('MUSIC_SONGS')          # (comma list: only these level songs)
    names = [n for n in LEVEL_MUSIC if not want or n in want.split(',') or n == MENU_SONG]
    extra = os.environ.get('MUSIC_EXTRA')
    names += [n for n in EXTRA if (n in extra.split(',') if extra is not None else n not in EXTRA_OFF)]
    ids = dict(MUSIC, **EXTRA)
    songs, samples, sample_idx = [], [], {}
    for name in names:
        aram, srcmap, (dst, n) = build_aram(rom, ids[name])
        assert IMG_LO <= dst and dst + n <= IMG_HI, (name, hex(dst), n)
        smp = []
        for s in range(NSRC):
            sid = srcmap.get(s)
            if sid is None:
                smp.append(0xFFFF)
                continue
            if sid not in sample_idx:
                sample_idx[sid] = len(samples)
                samples.append(sample_pcm(aram, s))
            smp.append(sample_idx[sid])
        songs.append((bytes(aram[IMG_LO:IMG_HI]), bytes(aram[0:0x600]), smp))
    buf = bytearray()
    head = 4 + 2 + 2 + 4 + 4 + 256 + 4 + 32
    buf += b'\0' * head
    off_songs = len(buf)
    buf += b'\0' * (96 * len(songs))
    off_samples = len(buf)
    buf += b'\0' * (12 * len(samples))
    # ARAM images deduplicated: ARAM 0..IMG_HI in 64-byte chunks, one shared pool (the engine, the
    # sound-effect block and most tables are the same in every song: 337 KB -> ~84 KB).  Song record:
    # off_img = its u16 chunk table [IMG_HI / 64], off_ram = the pool (sh/spceng.h spc_rdx)
    pool, pidx, tabs = bytearray(), {}, []
    for img, ram, smp in songs:
        full = ram + img
        assert len(full) == IMG_HI
        t = []
        for k in range(0, IMG_HI, 64):
            c = full[k:k + 64]
            if c not in pidx:
                pidx[c] = len(pool) // 64
                pool += c
            t.append(pidx[c])
        tabs.append(t)
    assert len(pidx) < 0x10000
    recs = []
    otabs = []
    for t in tabs:
        otabs.append(len(buf))
        buf += struct.pack(E + '%dH' % len(t), *t)
    buf += b'\0' * (-len(buf) % 16)
    o_pool = len(buf)
    buf += pool
    for (img, ram, smp), o_tab in zip(songs, otabs):
        recs.append(struct.pack(E + 'II', o_tab, o_pool) + struct.pack(E + '%dH' % NSRC, *smp) + b'\0' * 8)
    print('   music: %d songs, ARAM images %d KB -> %d KB (64-byte chunk pool)' % (
        len(songs), len(songs) * IMG_HI // 1024, (len(pool) + 2 * sum(map(len, tabs))) // 1024))
    srecs = []
    for pcm, n, loop in samples:
        buf += b'\0' * (-len(buf) % 4)
        o = len(buf)
        buf += struct.pack(E + '%dh' % len(pcm), *pcm)
        srecs.append(struct.pack(E + 'III', o, n, loop))
    level_song = bytearray([0xFF] * 256)
    id_song = bytearray([0xFF] * 32)
    for si, name in enumerate(names):
        for e in LEVEL_MUSIC.get(name, []):
            level_song[e] = si
        id_song[ids[name]] = si
    menu = bytes([names.index(MENU_SONG), 0, 0, 0])
    buf[0:head] = b'MUS1' + struct.pack(E + 'HHII', len(songs), len(samples), off_songs, off_samples) + \
        bytes(level_song) + menu + bytes(id_song)
    for i, r in enumerate(recs):
        assert len(r) == 96
        buf[off_songs + i * 96: off_songs + (i + 1) * 96] = r
    for i, r in enumerate(srecs):
        buf[off_samples + i * 12: off_samples + (i + 1) * 12] = r
    return bytes(buf), names


if __name__ == '__main__':
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
    from dkcrom import Rom
    rom = Rom(sys.argv[1])
    rom.check()
    if len(sys.argv) > 2 and sys.argv[2] == 'aram':
        a, srcmap, song = build_aram(rom, int(sys.argv[3], 0))
        open(sys.argv[4], 'wb').write(a)
        print('song block %04X+%04X, srcn %d' % (song[0], song[1], len(srcmap)))
    else:
        blob, names = build_music(rom, '<' if 'le' in sys.argv[2:] else '>')
        if len(sys.argv) > 2 and sys.argv[2] != 'le':
            open(sys.argv[2], 'wb').write(blob)
        print('music: %d songs, %d KB' % (len(names), len(blob) // 1024))
        for i, n in enumerate(names):
            print(i, n)
