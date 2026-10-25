#!/usr/bin/env python3
"""Check and normalise the user's Donkey Kong Country ROM before a build.

Accepts .sfc / .smc / .swc / .fig / .bin, with or without a 512-byte copier header, or a .zip
holding one of those.  Identifies the game, region and revision from the cartridge's own internal
header (no list of other dumps needed), verifies the internal checksum (catches bad dumps), and
writes the plain 4 MB image the build uses.

  romcheck.py INPUT OUT.sfc          exit 0: supported, 2: not supported (message on stdout)
"""
import hashlib, io, os, sys, zipfile

# images the port has been verified with (md5 of the headerless 4 MB image)
SUPPORTED = {'30c5f292ff4cbbfcc00fd8fa96c2de3b': 'USA 1.0'}
REGION = {0: 'Japan', 1: 'USA', 2: 'Europe', 3: 'Sweden', 6: 'France', 8: 'Spain', 9: 'Germany',
          10: 'Italy', 13: 'Netherlands', 15: 'Canada'}
TITLES = (b'DONKEY KONG COUNTRY', b'SUPER DONKEY KONG')


def read_input(path):
    if path.lower().endswith('.zip'):
        z = zipfile.ZipFile(path)
        names = [n for n in z.namelist() if os.path.splitext(n)[1].lower() in ('.sfc', '.smc', '.swc', '.fig', '.bin')]
        if len(names) != 1:
            raise SystemExit('The zip should contain exactly one SNES ROM (.sfc / .smc); found %d.' % len(names))
        return z.read(names[0])
    return open(path, 'rb').read()


def identify(data):
    """-> (image, info dict) or raise SystemExit with a readable reason"""
    if len(data) % 0x8000 == 0x200:              # copier header (SMC / SWC / FIG)
        data = data[0x200:]
    if len(data) != 0x400000:
        raise SystemExit('This file is %d KB; a Donkey Kong Country ROM is 4096 KB. Is it the right game?'
                         % (len(data) // 1024))
    h = data[0xFFC0:0xFFE0]
    title = h[:21].rstrip(b' \0')
    if not any(title.startswith(t) for t in TITLES):
        raise SystemExit('This ROM\'s header says "%s", not Donkey Kong Country.' % title.decode('ascii', 'replace'))
    region, rev = h[25], h[27]
    ck, comp = data[0xFFDE] | data[0xFFDF] << 8, data[0xFFDC] | data[0xFFDD] << 8
    total = sum(data) & 0xFFFF
    info = dict(title=title.decode('ascii', 'replace'), region=REGION.get(region, 'region %d' % region),
                revision='1.%d' % rev, md5=hashlib.md5(data).hexdigest(), checksum_ok=(total == ck and ck ^ comp == 0xFFFF))
    return data, info


def main():
    src, out = sys.argv[1], sys.argv[2]
    data, info = identify(read_input(src))
    name = '%s, %s, revision %s' % (info['title'], info['region'], info['revision'])
    if not info['checksum_ok']:
        print('Warning: the internal checksum does not match - the dump may be damaged (%s).' % name)
    ver = SUPPORTED.get(info['md5'])
    if not ver:
        print('Recognised %s, but this exact version is not supported yet.' % name)
        print('Supported so far: %s.  (md5 %s)' % (', '.join(SUPPORTED.values()), info['md5']))
        sys.exit(2)
    open(out, 'wb').write(data)
    print('ROM OK: %s (%s)' % (name, ver))


if __name__ == '__main__':
    main()
