#!/usr/bin/env python3
"""(I24) SOUND FM, step 3: a song's commands -> the Z80 driver's byte stream (z80/fmdrv.asm reads it).

Per channel stream (the channel is implied: phrases repeat byte for byte, across channels too):
  00-3F        WAIT n+1 ticks                      40 lo hi    WAIT n ticks
  42           END (the stream is over)            43          LOOP (back to the loop point; the 68000's)
  50 inst env fh fl vol  KEYON  fh = pan << 6 | block << 3 | fnum >> 8
  51 fh fl     FREQ                                52 vol      VOL (TL added to the carriers)
  53           KEYOFF                              54          ENV2 (the carriers' decay after a ramp)
  55 pan       PAN (bit 1 left, bit 0 right)       56 n b0..bn PSG: n + 1 raw bytes (n 0-7)
  57 d         VOL += d (signed)                   58 d        FREQ += d (signed, same block)
  59-5B        KEYOFF, then WAIT 1-3               5C-5F d      VOL += d, then WAIT 1-4
  80-FF        1 ww ddddd: FREQ += d (-16..15), then WAIT ww (0-3)
The 68000 resolves the CALLs (LZ77 matches into the song's own blob, ROM) - the Z80 never sees them:
  41 o.lo o.hi len    CALL: len bytes at blob offset o, then on here (nested up to CALL_DEPTH)
  60-7F o.lo o.hi     CALL of len = op - 0x5D (3-34 bytes)
(I24 round 2: waits folded into the commands before them, 3-byte CALLs, CALLs nested 8 deep - the 27
 songs 366 -> 192 KB)"""
import collections

CALL_DEPTH = 8
CAND = 48


NSTREAM = 7                     # FM 0-5, PSG


def tokens(song):
    """-> [[token bytes] per stream], [loop token index per stream]"""
    cmds = sorted(song.cmds, key=lambda e: (e[0], e[1]))
    per = [[] for _ in range(NSTREAM)]
    for k, seq, c in cmds:
        op = c[0]
        st = NSTREAM - 1 if op == 'psg' else c[1]
        per[st].append((k, c))
    outs, loops = [], []
    for st in range(NSTREAM):
        out = []
        last_f = None; last_v = None
        t = 0
        loop_tok = None
        ev = per[st]
        i = 0
        def wait(n):
            while n > 0:
                if n <= 64: out.append(bytes([n - 1])); n = 0
                else:
                    m = min(n, 0xFFFF); out.append(bytes([0x40, m & 255, m >> 8])); n -= m
        while True:
            if song.loop is not None and loop_tok is None and t >= song.loop:
                loop_tok = len(out); last_f = last_v = None     # (the loop is entered from two places)
            while i < len(ev) and ev[i][0] <= t:
                c = ev[i][1]; i += 1
                op = c[0]
                if op == 'keyon':
                    _, ch, iid, eid, fn, vol, pan = c
                    out.append(bytes([0x50, iid, eid, pan << 6 | fn >> 8, fn & 255, vol])); last_f, last_v = fn, vol
                elif op == 'freq':
                    fn = c[2]
                    if last_f is not None and (last_f >> 11) == (fn >> 11) and -128 <= fn - last_f <= 127:
                        out.append(bytes([0x58, (fn - last_f) & 255]))
                    else:
                        out.append(bytes([0x51, fn >> 8, fn & 255]))
                    last_f = fn
                elif op == 'vol':
                    v = c[2]
                    if last_v is not None and -128 <= v - last_v <= 127: out.append(bytes([0x57, (v - last_v) & 255]))
                    else: out.append(bytes([0x52, v]))
                    last_v = v
                elif op == 'keyoff': out.append(bytes([0x53]))
                elif op == 'env2': out.append(bytes([0x54]))
                elif op == 'pan': out.append(bytes([0x55, c[2]]))
                elif op == 'psg':
                    b = list(c[1])
                    while b:
                        q = b[:8]; b = b[8:]
                        out.append(bytes([0x56, len(q) - 1] + q))
                else:
                    raise ValueError(op)
            nxt = ev[i][0] if i < len(ev) else song.end
            if song.loop is not None and loop_tok is None and t < song.loop < nxt: nxt = song.loop
            if t >= song.end: break
            nxt = min(max(nxt, t + 1), song.end)
            wait(nxt - t)
            t = nxt
        out.append(bytes([0x43 if song.loop is not None else 0x42]))
        out, loop_tok = fold(out, loop_tok)
        outs.append(out); loops.append(loop_tok)
    return outs, loops


def fold(out, lt):
    """the short forms: a WAIT of 1-3 (4) ticks after FREQ += d / KEYOFF / VOL += d goes into the command;
    small FREQ steps take one byte (vibrato: 2 bytes a step -> 1).  Never across the loop point"""
    res, nlt, i = [], None, 0
    while i < len(out):
        if i == lt: nlt = len(res)
        t = out[i]
        nx = out[i + 1] if i + 1 < len(out) and i + 1 != lt else None
        w = nx[0] + 1 if nx is not None and nx[0] < 0x40 else 0
        op = t[0]
        if op == 0x58:
            d = (t[1] ^ 0x80) - 0x80
            if -16 <= d <= 15:
                ww = w if w <= 3 else 0
                res.append(bytes([0x80 | ww << 5 | (d & 31)])); i += 2 if ww else 1
                continue
        elif op == 0x53 and 1 <= w <= 3:
            res.append(bytes([0x58 + w])); i += 2
            continue
        elif op == 0x57 and 1 <= w <= 4:
            res.append(bytes([0x5B + w, t[1]])); i += 2
            continue
        res.append(t); i += 1
    if lt is not None and nlt is None: nlt = len(res)
    return res, nlt


def cmd_len(b, pos):
    """the length of the command at b[pos] (CALLs included)"""
    op = b[pos]
    if op < 0x40 or op >= 0x80: return 1
    if 0x60 <= op < 0x80: return 3
    if op == 0x56: return b[pos + 1] + 3
    return {0x40: 3, 0x41: 4, 0x42: 1, 0x43: 1, 0x50: 6, 0x51: 3, 0x52: 2, 0x53: 1, 0x54: 1, 0x55: 2,
            0x57: 2, 0x58: 2, 0x59: 1, 0x5A: 1, 0x5B: 1, 0x5C: 2, 0x5D: 2, 0x5E: 2, 0x5F: 2}[op]


def compress_all(streams, loops):
    """all streams into one blob (shared dictionary) -> (blob, [start offsets], [loop offsets])"""
    blob = bytearray(); starts = []; lofs = []
    ctx = None
    for toks, lt in zip(streams, loops):
        z, lo, ctx = compress(toks, lt, base=len(blob), ctx=ctx)
        starts.append(len(blob)); lofs.append(lo + len(blob) if lo is not None else None)
        blob += z
    return bytes(blob), starts, lofs


def call_cost(ln):
    return 3 if ln <= 34 else 4


def compress(toks, loop_tok, minlen=4, base=0, ctx=None):
    """greedy LZ77 over whole commands: a CALL refers to earlier OUTPUT items (literals / calls) that
    together cover a run of the original commands exactly (also an earlier stream's: ctx).
    -> (bytes, loop offset, ctx)"""
    if ctx is None: ctx = dict(items=[], start_at={}, end_at={}, by3=collections.defaultdict(list), tb=[])
    items, start_at, end_at, by3, tb = ctx['items'], ctx['start_at'], ctx['end_at'], ctx['by3'], ctx['tb']
    out = bytearray()
    g0 = len(tb)                    # (global token index of this stream's first)
    tb.extend(toks)
    n = len(tb)
    i = g0
    loop_tok = None if loop_tok is None else loop_tok + g0
    loop_off = None
    while i < n:
        if i == loop_tok: loop_off = len(out)
        best = None
        if i + 2 < n and (loop_tok is None or not (i < loop_tok < i + 3)):
            key = (tb[i], tb[i + 1], tb[i + 2])
            for j in reversed(by3.get(key, [])[-CAND:]):
                # extend the match
                m = 0
                while i + m < n and j + m < i and tb[j + m] == tb[i + m] and (loop_tok is None or not (i < loop_tok <= i + m)):
                    m += 1
                    if tb[j + m - 1][0] in (0x42, 0x43): break
                # cut back to an item boundary that ends a run starting at j
                while m > 0 and (j + m) not in end_at: m -= 1
                if m < 3: continue
                a, b = items[start_at[j]][2], items[end_at[j + m]][3]
                if b - a < minlen or b - a > 255: 
                    # (too long for len8: shorten to an item end within 255 bytes)
                    while m > 0 and ((j + m) not in end_at or items[end_at[j + m]][3] - a > 255): m -= 1
                    if m < 3: continue
                    b = items[end_at[j + m]][3]
                    if b - a < minlen: continue
                dep = 1 + max(it[4] for it in items[start_at[j]:end_at[j + m] + 1])
                if dep > CALL_DEPTH: continue
                if a > 0xFFFF: continue
                if best is None or (b - a) > best[0]: best = (b - a, j, m, a, dep)
        if best and best[0] > call_cost(best[0]):
            ln, j, m, a, dep = best
            o0 = base + len(out)
            out += bytes([0x5D + ln, a & 255, a >> 8]) if ln <= 34 else bytes([0x41, a & 255, a >> 8, ln])
            items.append((i, i + m, o0, base + len(out), dep))
            start_at.setdefault(i, len(items) - 1); end_at[i + m] = len(items) - 1
            for q in range(i, i + m):
                if q + 2 < n and q > i: pass
            for q in range(i, i + m - 2):
                by3[(tb[q], tb[q + 1], tb[q + 2])].append(q) if q in start_at else None
            i += m
            continue
        o0 = base + len(out)
        out += tb[i]
        items.append((i, i + 1, o0, base + len(out), 0))
        start_at.setdefault(i, len(items) - 1); end_at[i + 1] = len(items) - 1
        if i + 2 < n: by3[(tb[i], tb[i + 1], tb[i + 2])].append(i)
        i += 1
    if loop_tok is not None and loop_off is None: loop_off = len(out)
    return bytes(out), loop_off, ctx


def expand(blob, start, limit=10 ** 7):
    """decode one stream (as the 68000 does) -> [(command bytes)] up to its END / LOOP"""
    out = []
    stack = []
    pos, end = start, len(blob)
    while len(out) < limit:
        if stack and pos >= end:
            pos, end = stack.pop()
            continue
        op = blob[pos]
        L = cmd_len(blob, pos)
        c = blob[pos:pos + L]
        if op == 0x41 or 0x60 <= op < 0x80:
            stack.append((pos + L, end))
            assert len(stack) <= CALL_DEPTH
            o = c[1] | c[2] << 8
            pos, end = o, o + (c[3] if op == 0x41 else op - 0x5D)
            continue
        out.append(bytes(c))
        pos += L
        if op in (0x42, 0x43): break
    return out
