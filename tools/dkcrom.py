"""Helpers for reading Donkey Kong Country (USA v1.0) data out of the user's own ROM.

Formats were worked out from Yoshifanatic1's DKC1 disassembly:
  * CODE_B896FC  - BG graphics decompressor
  * CODE_818C66  - per-tileset layout / Map32 pointers
  * CODE_818CEF / CODE_818DFA - tilemap streaming (chunk + flip format)
"""
import hashlib

EXPECTED_MD5 = "30c5f292ff4cbbfcc00fd8fa96c2de3b"


class Rom:
    def __init__(self, path):
        data = open(path, "rb").read()
        if len(data) % 0x8000 == 0x200:  # strip copier header
            data = data[0x200:]
        self.data = data
        self.md5 = hashlib.md5(data).hexdigest()

    def check(self):
        if self.md5 != EXPECTED_MD5:
            raise SystemExit(
                "ROM MD5 %s does not match Donkey Kong Country (USA) v1.0 (%s)"
                % (self.md5, EXPECTED_MD5))

    @staticmethod
    def off(addr):
        """HiROM SNES address -> file offset."""
        return addr & 0x3FFFFF

    def read(self, addr, n):
        o = self.off(addr)
        return self.data[o:o + n]

    def u8(self, addr):
        return self.data[self.off(addr)]

    def u16(self, addr):
        o = self.off(addr)
        return self.data[o] | (self.data[o + 1] << 8)

    def u24(self, addr):
        o = self.off(addr)
        return self.data[o] | (self.data[o + 1] << 8) | (self.data[o + 2] << 16)

    def decompress(self, addr):
        """DKC1 BG decompressor (port of CODE_B89700).

        Stream header: 64 little-endian words (a dictionary), then commands:
          00            end
          00nnnnnn      copy n literal bytes            (n = 1..63)
          01nnnnnn v    repeat byte v n times
          10nnnnnn lo hi copy n bytes from out[hi:lo]
          11iiiiii      emit dictionary word i (2 bytes)
        """
        base = self.off(addr)
        d = self.data
        y = base + 0x80
        out = bytearray()
        while True:
            b = d[y]
            if b == 0:
                break
            y += 1
            cmd = b & 0xC0
            n = b & 0x3F
            if cmd == 0x00:
                out += d[y:y + n]
                y += n
            elif cmd == 0x40:
                out += bytes([d[y]]) * n
                y += 1
            elif cmd == 0x80:
                src = d[y] | (d[y + 1] << 8)
                y += 2
                for i in range(n):
                    out.append(out[src + i])
            else:
                w = base + n * 2
                out += d[w:w + 2]
        return bytes(out)


def snes_color(w):
    r = (w & 31) << 3
    g = ((w >> 5) & 31) << 3
    b = ((w >> 10) & 31) << 3
    return (r | r >> 5, g | g >> 5, b | b >> 5)


def decode_4bpp_tile(buf, off):
    """Return 64 pixel values (0..15) for an SNES 4bpp planar tile."""
    px = [0] * 64
    for row in range(8):
        p0 = buf[off + row * 2]
        p1 = buf[off + row * 2 + 1]
        p2 = buf[off + 16 + row * 2]
        p3 = buf[off + 16 + row * 2 + 1]
        for col in range(8):
            bit = 7 - col
            px[row * 8 + col] = (((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1)
                                 | (((p2 >> bit) & 1) << 2) | (((p3 >> bit) & 1) << 3))
    return px


def decode_2bpp_tile(buf, off):
    px = [0] * 64
    for row in range(8):
        p0 = buf[off + row * 2]
        p1 = buf[off + row * 2 + 1]
        for col in range(8):
            bit = 7 - col
            px[row * 8 + col] = ((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1)
    return px
