#!/usr/bin/env python3
"""(I24) SOUND FM, step 1: play a song of the music pack through the game's own sound driver (translated by
tools/audio/spc2x.py from the user's ROM, Python backend) and log every S-DSP register write with its time.
Same memory model and timing as the 32X player (sh/musplay.c mp_start / mp_render, sh/spceng.h).

  fmlog.py rom.sfc music_le.bin song seconds out.log
  log: "# srcn sample" lines (the song's sample map), then "t reg val" (t = 32 kHz samples, hex)

(tools/audio/fmdump.c is the C twin used in development; it also renders WAVs.)"""
import importlib.util, os, struct, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
from spcrun import SPC

IMG_HI = 0x3200
NSRC = 40


def engine(rom, cache):
    if not os.path.exists(cache) or os.path.getmtime(cache) < os.path.getmtime(rom):
        os.makedirs(os.path.dirname(cache) or '.', exist_ok=True)
        subprocess.check_call([sys.executable, os.path.join(HERE, '..', 'spc2x.py'), rom, 'py', cache],
                              stdout=subprocess.DEVNULL)
    spec = importlib.util.spec_from_file_location('dkc_engine_fm', cache)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class SPC32(SPC):
    """the 32X player's memory: RAM $0000-$05FF, the song image (read only) to $31FF, zero above"""
    def rd(self, a):
        if a >= IMG_HI and not (0xF0 <= a <= 0xFF): return 0
        return SPC.rd(self, a)

    def wr(self, a, v):
        if a == 0xF3 or a < 0x600: SPC.wr(self, a, v)


def song_aram(pack, song):
    nsongs, nsamples, off_songs, off_samples = struct.unpack_from('<HHII', pack, 4)
    o_tab, o_pool = struct.unpack_from('<II', pack, off_songs + 96 * song)
    smp = struct.unpack_from('<%dH' % NSRC, pack, off_songs + 96 * song + 8)
    tab = struct.unpack_from('<%dH' % (IMG_HI // 64), pack, o_tab)
    aram = bytearray(0x10000)
    for k, c in enumerate(tab):
        aram[k * 64:k * 64 + 64] = pack[o_pool + c * 64:o_pool + c * 64 + 64]
    return aram, smp


def ticks(gen, pack, song, seconds):
    """-> (sample map {srcn: pack sample}, [(t32, [(reg, val), ...]) per engine tick]) - the engine's own
    timing (4 x $EC samples at 32 kHz a tick)"""
    aram, smp = song_aram(pack, song)
    s = SPC32(aram, bytes(128), gen)
    smap = {i: g for i, g in enumerate(smp) if g != 0xFFFF}
    s.stack = ['__end__']; s.run('CODE_1071', '__end__')
    out = [(0, list(s.log))]
    s.m[0x24] = 1
    t, total = 0, int(seconds * 32000)
    while t < total:
        s.log = []
        s.stack = ['__end__']; s.run('CODE_064F', 'CODE_05FE')
        out.append((t, list(s.log)))
        t += 4 * (s.m[0xEC] or 256)
    return smap, out


def run(gen, pack, song, seconds, out):
    aram, smp = song_aram(pack, song)
    s = SPC32(aram, bytes(128), gen)
    with open(out, 'w') as o:
        for i, g in enumerate(smp):
            if g != 0xFFFF: o.write('# %d %d\n' % (i, g))
        def call(label, stop='CODE_05FE'):
            s.stack = ['__end__']
            s.run(label, stop if stop else '__end__')
        call('CODE_1071', None)
        s.m[0x24] = 1
        t, total = 0, int(seconds * 32000)
        lines = []
        for r, v in s.log: lines.append('0 %02X %02X\n' % (r, v))
        while t < total:
            s.log = []
            call('CODE_064F')
            for r, v in s.log: lines.append('%d %02X %02X\n' % (t, r, v))
            ec = s.m[0xEC] or 256
            t += 4 * ec
        o.writelines(lines)


if __name__ == '__main__':
    rom, packf, song, secs, out = sys.argv[1:6]
    gen = engine(rom, os.environ.get('FMLOG_ENGINE', os.path.join(os.path.dirname(os.path.abspath(out)), 'dkc_engine_fm.py')))
    run(gen, open(packf, 'rb').read(), int(song), float(secs), out)
