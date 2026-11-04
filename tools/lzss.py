"""(I21q) LZSS for the Genesis plane data the 68000 loads (md/main.c lz_*): per 8 items a flag byte (bit 7
first; 1 = a literal byte, 0 = a match: 2 bytes, big-endian (distance - 1) << 4 | (length - 3), distance
1-4096, length 3-18).  Lossless; the 68000 keeps the last 4 KB in a ring buffer."""
def compress(data, W=4096, MINL=3, MAXL=18):
    out = bytearray(); i = 0; n = len(data)
    head = {}
    while i < n:
        flagpos = len(out); out.append(0); flags = 0
        for bit in range(8):
            if i >= n: break
            best_l = 0; best_d = 0
            key = data[i:i+3]
            if len(key) == 3:
                for j in reversed(head.get(key, [])[-64:]):
                    if i - j > W: break
                    l = 0
                    while l < MAXL and i + l < n and data[j + l] == data[i + l]: l += 1
                    if l > best_l: best_l, best_d = l, i - j
                    if l == MAXL: break
            if best_l >= MINL:
                v = (best_d - 1) << 4 | (best_l - MINL)
                out += bytes([v >> 8, v & 255]); adv = best_l
            else:
                flags |= 0x80 >> bit; out.append(data[i]); adv = 1
            for k in range(adv):
                if i + k + 3 <= n: head.setdefault(data[i+k:i+k+3], []).append(i + k)
            i += adv
        out[flagpos] = flags
    return bytes(out)
def decompress(z, n):
    o = bytearray(); i = 0
    while len(o) < n:
        f = z[i]; i += 1
        for bit in range(8):
            if len(o) >= n: break
            if f & (0x80 >> bit): o.append(z[i]); i += 1
            else:
                v = z[i] << 8 | z[i+1]; i += 2; d = (v >> 4) + 1; l = (v & 15) + 3
                for _ in range(l): o.append(o[-d])
    return bytes(o)
