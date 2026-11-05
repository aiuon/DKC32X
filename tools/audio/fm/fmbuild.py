#!/usr/bin/env python3
"""(I24) SOUND FM at build time: every song of the user's ROM -> the Z80 driver's data.

  fmbuild.py rom.sfc outdir

outdir/fm.json   per song: the samples the 32X plays (SRCN mask), the tick grid offset; the samples SOUND
                 FM never mixes ('swap': mkmusic.py puts them last - an FM page replaces them)
outdir/song_NN.bin   the song blocks (md/fm.c reads them): 'FMS1', u16 ninst, u16 nenv, u32 0,
                 u32 off_inst, off_env, off_blob, u16 start[7], u16 loop[7] (0xFFFF none), the tables, the blob
outdir/fitcache.json  the FM patch fits (slow): kept while the samples are the same (FM_FITCACHE: another
                 place - tools/convert.py keeps it in cache/, made on this machine from this ROM, never shipped)

Nothing here ships: it is made from the ROM on the user's machine (the converter does, the data is not in
the bundle)."""
import hashlib, json, os, struct, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..'))
sys.path.insert(0, os.path.join(HERE, '..', '..'))
import fmlog, fmsong, fmpack, fmexport, fmconv, fmfit, mdconv
import collections
import numpy as np
from samples import Pack

SECONDS = 600            # the log's length: long enough to see the longest loop repeat twice
SFX_TICKS = 120          # a sound effect's run in the scan (1.5 s)


def song_block(S):
    streams, lts = fmpack.tokens(S)
    blob, starts, lofs = fmpack.compress_all(streams, lts)
    for st, toks in zip(starts, streams):       # (decode check: the 68000 reads it this way)
        assert fmpack.expand(blob, st) == toks
    ib, eb = fmexport.inst_bytes(S), fmexport.env_bytes(S)
    assert len(S.inst) <= 32 and len(S.env) <= 146, (len(S.inst), len(S.env))
    hdr_n = 52
    off_inst = hdr_n
    off_env = off_inst + len(ib)
    off_blob = off_env + len(eb)
    assert len(blob) < 0x10000
    h = b'FMS1' + struct.pack('>HHI', len(S.inst), len(S.env), 0) + struct.pack('>III', off_inst, off_env, off_blob)
    h += struct.pack('>7H', *starts) + struct.pack('>7H', *[0xFFFF if x is None else x for x in lofs])
    assert len(h) == hdr_n
    return h + ib + eb + blob


def song_block_md(S, dtoks, dlt):
    """(I25) SOUND MD's song block: 'FMS2' - the 7 Z80 streams (5 FM channels) + the PCM stream (mdconv.py),
    one LZ blob; header as FMS1 with 8 starts / loops (56 bytes)"""
    streams, lts = fmpack.tokens(S)
    streams = list(streams) + [dtoks]; lts = list(lts) + [dlt]
    blob, starts, lofs = fmpack.compress_all(streams, lts)
    for st, toks in zip(starts, streams):
        assert fmpack.expand(blob, st) == toks
    ib, eb = fmexport.inst_bytes(S), fmexport.env_bytes(S)
    assert len(S.inst) <= 32 and len(S.env) <= 146, (len(S.inst), len(S.env))
    hdr_n = 56
    off_inst = hdr_n
    off_env = off_inst + len(ib)
    off_blob = off_env + len(eb)
    assert len(blob) < 0x10000
    h = b'FMS2' + struct.pack('>HHI', len(S.inst), len(S.env), 0) + struct.pack('>III', off_inst, off_env, off_blob)
    h += struct.pack('>8H', *starts) + struct.pack('>8H', *[0xFFFF if x is None else x for x in lofs])
    assert len(h) == hdr_n
    return h + ib + eb + blob


def md_job(a):
    """(a worker, pass 4) one song for SOUND MD: 5 FM channels + the PCM stream -> outdir/md_song_NN.bin"""
    rom_path, outdir, song, cache, order = a
    t0 = time.time()
    le, pack, gen, smap, tl, ev = _log(rom_path, outdir, song)
    nt = ev[-1][0] + 1
    lp = fmsong.find_loop(ev, nt)
    S = fmsong.convert(pack, smap, ev, nt, cache, lp, n_fm=5)
    notes = fmconv.notes_of(ev)
    bank = mdconv.Bank(pack, order)
    st = collections.Counter()
    dtoks, dlt = mdconv.drum_stream(S, notes, smap, bank, st)
    assert len(bank.ent) == len(order), 'a drum sample outside the bank'
    blk = song_block_md(S, dtoks, dlt)
    open(os.path.join(outdir, 'md_song_%02d.bin' % song), 'wb').write(blk)
    return dict(song=song, size=len(blk), att=mdconv.sfx_att(mdconv.song_mv(notes)), stats=dict(st),
                secs=time.time() - t0)


def sfx_samples(gen, pack_bytes, song):
    """the samples sound effects play with this song's ARAM, by running every effect command through the engine"""
    aram, smp = fmlog.song_aram(pack_bytes, song)
    used = set()
    for cmd in range(1, 0xFC):
        s = fmlog.SPC32(bytearray(aram), bytes(128), gen)
        s.stack = ['__end__']; s.run('CODE_1071', '__end__'); s.m[0x24] = 1
        s.m[0x4D8] = 0; s.a = cmd; s.x = 0
        s.stack = ['__end__']
        try:
            s.run('CODE_1173', '__end__')
        except Exception:
            continue
        if not any(s.m[0x1A0 + i] for i in range(8)): continue
        for k in range(SFX_TICKS):
            s.log = []; s.stack = ['__end__']
            try:
                s.run('CODE_064F', 'CODE_05FE')
            except Exception:
                break
            for r, v in s.log:
                if r == 0x4C:
                    for i in range(8):
                        if v >> i & 1 and s.m[0x1A0 + i]: used.add(s.dsp[i * 16 + 4])
            if not any(s.m[0x1A0 + i] for i in range(8)): break
    return set(smp[x] for x in used if smp[x] != 0xFFFF)


def _log(rom_path, outdir, song):
    lep = os.path.join(outdir, 'music_le.bin')
    le = open(lep, 'rb').read()
    pack = Pack(lep)
    gen = fmlog.engine(rom_path, os.path.join(outdir, 'dkc_engine_fm.py'))
    smap, tl = fmlog.ticks(gen, le, song, SECONDS)
    ev = fmsong.events_from_ticks(tl)
    return le, pack, gen, smap, tl, ev


def req_job(a):
    """(a worker, pass 1) the fits a song asks for"""
    rom_path, outdir, song, cache = a
    le, pack, gen, smap, tl, ev = _log(rom_path, outdir, song)
    return song, fmsong.fit_requests(pack, smap, ev, cache), cache.get('info', {})


FIT_SEEDS = (1, 2, 3, 4)       # (each sample fitted from these starts, the best kept: the search's luck
                                #  varies ~15 % a start - best of 4 ~ 20 % better than one)


def fit_job(a):
    """(a worker, pass 2) one sample's FM patch from one start of the search"""
    outdir, gi, fplay, info, seed = a
    pack = Pack(os.path.join(outdir, 'music_le.bin'))
    pcm, loop = pack.sample(gi)
    alg, fb, mul, tl, err, _ = fmfit.fit(pcm, loop, info['f0'], fplay, rng=fmfit.Rng(seed))
    _, rms = fmfit.simulate(alg, fb, np.array([mul]), np.array([tl], float), fplay, rms=True)
    return gi, seed, dict(alg=alg, fb=fb, mul=mul, tl=tl, err=round(err, 6), rms=round(float(rms[0]), 9),
                          fplay=fplay, seed=seed)


def song_job(a):
    """(a worker, pass 3) one song: the conversion (every fit made), its block (outdir/song_NN.bin), its effects'
    samples"""
    rom_path, outdir, song, cache = a
    t0 = time.time()
    le, pack, gen, smap, tl, ev = _log(rom_path, outdir, song)
    nt = ev[-1][0] + 1
    lp = fmsong.find_loop(ev, nt)
    S = fmsong.convert(pack, smap, ev, nt, cache, lp)
    blk = song_block(S)
    open(os.path.join(outdir, 'song_%02d.bin' % song), 'wb').write(blk)
    mask, spc = 0, set()
    for srcn, r in S.role.items():
        if r == 'spc':
            mask |= 1 << srcn
            if smap.get(srcn) is not None: spc.add(smap[srcn])
    return dict(song=song, spc_mask=mask, grid=fmsong.grid_offset(tl), loop=lp, size=len(blk), spc=sorted(spc),
                sfx=sorted(sfx_samples(gen, le, song)), secs=time.time() - t0)


def _run(jobs, fn, n):
    if n > 1 and len(jobs) > 1:
        import multiprocessing
        with multiprocessing.Pool(min(n, len(jobs))) as pool:
            for r in pool.imap_unordered(fn, jobs):
                yield r
    else:
        for j in jobs: yield fn(j)


def build(rom_path, outdir):
    """-> the fm dict (fm.json); the song blocks in outdir.  Same songs as build_viewer's music (MUSIC_SONGS).
    The songs are converted in parallel (FM_JOBS processes, default: the CPUs)"""
    os.makedirs(outdir, exist_ok=True)
    t0 = time.time()
    from dkcrom import Rom
    import mkmusic
    rom = Rom(rom_path)
    le, names = mkmusic.build_music(rom, '<')
    # (made before from the same music and the same converter: kept)
    h = hashlib.md5(le)
    for f in sorted(os.listdir(HERE)) + ['../spc2x.py', '../mkmusic.py']:
        if f.endswith('.py'): h.update(open(os.path.join(HERE, f), 'rb').read())
    bsig = h.hexdigest()
    jp = os.path.join(outdir, 'fm.json')
    if os.path.exists(jp):
        try:
            old = json.load(open(jp))
            if old.get('sig') == bsig and old.get('names') == names and all(
                    os.path.exists(os.path.join(outdir, b)) for b in old['blocks'].values()):
                print('   FM: %d songs (made before, kept)' % len(names))
                old.pop('swap_off', None)
                return old
        except (ValueError, KeyError):
            pass
    lep = os.path.join(outdir, 'music_le.bin')
    open(lep, 'wb').write(le)
    pack = Pack(lep)
    fmlog.engine(rom_path, os.path.join(outdir, 'dkc_engine_fm.py'))     # (made once, before the workers)
    # (the fits are slow: cached while the samples are the same)
    hs = hashlib.md5(le[pack.off_samples:])
    for f in ('fmfit.py', 'samples.py', 'fmconv.py'):   # (and the fitting code: a fit of an older one
        hs.update(open(os.path.join(HERE, f), 'rb').read())      #  is not reused)
    import inspect                                               # (of this file: the fit job and its seeds only -
    hs.update((inspect.getsource(fit_job) + repr(FIT_SEEDS)).encode())   #  I25: edits elsewhere kept the fits)
    sig = hs.hexdigest()
    cpath = os.environ.get('FM_FITCACHE') or os.path.join(outdir, 'fitcache.json')
    os.makedirs(os.path.dirname(os.path.abspath(cpath)), exist_ok=True)
    cache = {}
    if os.path.exists(cpath):
        try:
            c = json.load(open(cpath))
            if c.get('sig') == sig: cache = c
        except ValueError:
            pass
    cache['sig'] = sig
    for k in ('info', 'patch'): cache.setdefault(k, {})
    fm = {'songs': {}, 'blocks': {}, 'names': names, 'sig': bsig}
    n = int(os.environ.get('FM_JOBS') or os.cpu_count() or 1)
    # pass 1: the fits each song asks for; pass 2: each sample fitted once (at the frequency of the first song
    # that plays it - the same patches whatever the order or number of workers); pass 3: the songs
    reqs = {}
    for song, req, info in _run([(rom_path, outdir, song, cache) for song in range(pack.nsongs)], req_job, n):
        reqs[song] = req
        cache['info'].update(info)
    want = {}
    for song in sorted(reqs):
        for gi, fplay in reqs[song].items():
            if str(gi) not in cache['patch'] and gi not in want: want[gi] = fplay
    if want: print('   FM: fitting %d instruments' % len(want), flush=True)
    got = {}
    for gi, seed, p in _run([(outdir, gi, fp, cache['info'][str(gi)], sd) for gi, fp in sorted(want.items())
                             for sd in FIT_SEEDS], fit_job, n):
        got[(gi, seed)] = p
    for gi in want:
        cache['patch']['%d' % gi] = min((got[(gi, sd)] for sd in FIT_SEEDS), key=lambda p: (p['err'], p['seed']))
    json.dump(cache, open(cpath, 'w'))
    res = []
    for r in _run([(rom_path, outdir, song, cache) for song in range(pack.nsongs)], song_job, n):
        res.append(r)
        print('   FM %2d %-20s %5d bytes (%.0f s)' % (r['song'], names[r['song']], r['size'], r['secs']), flush=True)
    spc_used, sfx = set(), set()
    for r in sorted(res, key=lambda r: r['song']):
        song = r['song']
        spc_used |= set(r['spc'])
        sfx |= set(r['sfx'])
        fm['songs'][str(song)] = dict(spc_mask=r['spc_mask'], grid=r['grid'], name=names[song], loop=r['loop'],
                                      size=r['size'])
        fm['blocks'][str(song)] = 'song_%02d.bin' % song
    json.dump(cache, open(cpath, 'w'))
    # (I25) SOUND MD: the effects (once: with the first song's ARAM - the effects are the same in every song
    # but for the master volume, which the 68000 applies), the PCM bank (effects' + drums' samples), the songs
    t1 = time.time()
    gen = fmlog.engine(rom_path, os.path.join(outdir, 'dkc_engine_fm.py'))
    fx = mdconv.all_sfx(gen, le, 0)
    sgi = set()
    for cmd, (nts, smap) in fx.items():
        for nt_ in nts:
            if nt_.get('noise'): sgi.add(mdconv.NOISE)
            elif smap.get(nt_['srcn']) is not None: sgi.add(smap[nt_['srcn']])
    order = sorted(sgi | spc_used | {mdconv.NOISE})
    bank = mdconv.Bank(pack, order)
    st = collections.Counter()
    fxs = {}
    for cmd, (nts, smap) in sorted(fx.items()):
        b = mdconv.sfx_stream(nts, smap, bank, st)
        if b: fxs[cmd] = b
    assert len(bank.ent) == len(order)
    open(os.path.join(outdir, 'md_sfx.bin'), 'wb').write(mdconv.sfx_table(fxs))
    open(os.path.join(outdir, 'md_bank.bin'), 'wb').write(bank.blob())
    md = {'songs': {}, 'blocks': {}, 'bank': 'md_bank.bin', 'sfx': 'md_sfx.bin', 'ta_units': mdconv.TA_UNITS,
          'md_att': mdconv.MD_ATT}
    for r in _run([(rom_path, outdir, song, cache, order) for song in range(pack.nsongs)], md_job, n):
        md['songs'][str(r['song'])] = dict(size=r['size'], att=r['att'])
        md['blocks'][str(r['song'])] = 'md_song_%02d.bin' % r['song']
        print('   MD %2d %-20s %5d bytes %s (%.0f s)' % (r['song'], names[r['song']], r['size'], r['stats'], r['secs']),
              flush=True)
    fm['md'] = md
    print('   MD: %d effects (%d KB), PCM bank %d samples (%d KB), %d KB of song data, %.0f s' % (
        len(fxs), os.path.getsize(os.path.join(outdir, 'md_sfx.bin')) // 1024, len(order),
        os.path.getsize(os.path.join(outdir, 'md_bank.bin')) // 1024,
        sum(e['size'] for e in md['songs'].values()) // 1024, time.time() - t1))
    keep = sfx | spc_used
    fm['swap'] = sorted(g for g in range(pack.nsamples) if g not in keep)
    fm['sfx'] = sorted(sfx)
    json.dump(fm, open(jp, 'w'), indent=1)
    print('   FM: %d songs, %d KB of song data, %d samples only for the 32X music (FM pages replace them), %.0f s' % (
        pack.nsongs, sum(e['size'] for e in fm['songs'].values()) // 1024, len(fm['swap']), time.time() - t0))
    return fm


def main():
    build(sys.argv[1], sys.argv[2])


if __name__ == '__main__':
    main()
