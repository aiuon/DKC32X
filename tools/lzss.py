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


def compress_opt(data, W=4096, MINL=3, MAXL=18):
    """(I24) the same format, optimal parse (fewest bits: a literal 9, a match 17) - for the 68000's RAM image"""
    n = len(data)
    head = {}
    cand = [None] * n                      # per position: [(length, distance)] - the longest per length
    for i in range(n):
        key = data[i:i + 3]
        ms = []
        if len(key) == 3:
            best = 0
            for j in reversed(head.get(key, [])):
                if i - j > W: break
                l = 0
                while l < MAXL and i + l < n and data[j + l] == data[i + l]: l += 1
                if l > best:
                    for L in range(max(best + 1, MINL), l + 1): ms.append((L, i - j))
                    best = l
                if l == MAXL: break
            head.setdefault(key, []).append(i)
        cand[i] = ms
    INF = 1 << 60
    cost = [INF] * (n + 1); how = [None] * (n + 1)
    cost[n] = 0
    for i in range(n - 1, -1, -1):
        c = 9 + cost[i + 1]; h = None
        for L, d in cand[i]:
            if i + L <= n and 17 + cost[i + L] < c: c = 17 + cost[i + L]; h = (L, d)
        cost[i] = c; how[i] = h
    out = bytearray(); i = 0
    while i < n:
        flagpos = len(out); out.append(0); flags = 0
        for bit in range(8):
            if i >= n: break
            h = how[i]
            if h:
                v = (h[1] - 1) << 4 | (h[0] - MINL)
                out += bytes([v >> 8, v & 255]); i += h[0]
            else:
                flags |= 0x80 >> bit; out.append(data[i]); i += 1
        out[flagpos] = flags
    return bytes(out)
