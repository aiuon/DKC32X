#!/usr/bin/env python3
"""Check a folder holds no data from the ROM: every file (and the inside of .gz/.zip files, and hex
byte lists in text files) is searched for any run of >= 32 bytes that also occurs in the ROM
(runs that are just padding / repeated bytes are ignored).

  scanbundle.py ROM DIR        exit 1 if anything is found"""
import gzip, os, re, sys, zipfile
import numpy as np

MINRUN = 32


def rom_index(rom):
    a = np.frombuffer(rom[:len(rom) // 8 * 8], dtype='>u8')
    b = np.frombuffer(rom[:len(rom) // 8 * 8], dtype=np.uint8).reshape(-1, 8)
    keep = np.array([len(set(r)) >= 5 for r in b.tolist()])     # skip padding-like blocks
    return np.unique(a[keep])


def trivial(run):
    """padding, counting tables (0,1,2,.. / 0,n,0,n+1,..) and other runs any program can contain"""
    if len(set(run)) < 8:
        return True
    for st in (1, 2, 4):
        if len(set((run[i + st] - run[i]) & 255 for i in range(len(run) - st))) <= 3:
            return True
    return False


def find(data, idx, rom):
    hits = []
    if len(data) < MINRUN:
        return hits
    buf = np.frombuffer(data, dtype=np.uint8)
    for k in range(8):
        n = (len(buf) - k) // 8
        if n <= 0:
            continue
        v = buf[k:k + n * 8].view('>u8')
        pos = np.searchsorted(idx, v)
        pos[pos >= len(idx)] = 0
        seen = set()
        for i in np.nonzero(idx[pos] == v)[0]:
            off = k + 8 * int(i)
            blk = data[off:off + 8]
            r = rom.find(blk)
            while r >= 0:
                # grow the match both ways
                s, e = 0, 8
                while off - s > 0 and r - s > 0 and data[off - s - 1] == rom[r - s - 1]:
                    s += 1
                while off + e < len(data) and r + e < len(rom) and data[off + e] == rom[r + e]:
                    e += 1
                if (off - s) in seen:
                    break
                if s + e >= MINRUN and not trivial(data[off - s:off + e]):
                    hits.append((off - s, r - s, s + e))
                    seen.add(off - s)
                    break
                r = rom.find(blk, r + 1)
    return hits


HEX = re.compile(rb'(?:(?:0x|\$)?[0-9A-Fa-f]{2}[ ,]+){%d,}' % MINRUN)


def views(path, data):
    yield path, data
    if path.endswith('.gz'):
        yield path + '!gunzip', gzip.decompress(data)
    if path.endswith('.zip'):
        z = zipfile.ZipFile(path)
        for n in z.namelist():
            yield path + '!' + n, z.read(n)
    if not data[:4096].count(0):
        for m in HEX.finditer(data):
            bs = bytes(int(t[-2:], 16) for t in re.findall(rb'(?:0x|\$)?[0-9A-Fa-f]{2}', m.group(0)))
            yield '%s@%d(hex)' % (path, m.start()), bs


def main():
    rom = open(sys.argv[1], 'rb').read()
    if len(rom) % 0x8000 == 0x200:
        rom = rom[0x200:]
    idx = rom_index(rom)
    bad = nfiles = 0
    for dp, dns, fns in os.walk(sys.argv[2]):
        for f in fns:
            p = os.path.join(dp, f)
            nfiles += 1
            for name, d in views(p, open(p, 'rb').read()):
                h = find(d, idx, rom)
                if h:
                    bad += 1
                    print('ROM DATA: %s: %d run(s), e.g. %d bytes at file+%X = ROM+%X' % (name, len(h), h[0][2], h[0][0], h[0][1]))
    print('scanned %d files: %s' % (nfiles, 'CLEAN' if not bad else '%d with ROM data' % bad))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
