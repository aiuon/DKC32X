"""BRR decoding (blargg SPC_DSP rules) and 8-bit normalization for the 32X player."""


def decode_brr(aram, start, loop_addr):
    """-> (samples16 list, loop_index or None).  Follows the block chain from 'start' until an end
    block; if it loops, the loop target block is located by address."""
    out, block_at = [], {}
    p1 = p2 = 0
    a = start
    loop_idx = None
    for _ in range(4096):                      # safety: 64K samples
        block_at[a] = len(out)
        hdr = aram[a]
        shift, filt = hdr >> 4, hdr & 0x0C
        for i in range(8):
            byte = aram[(a + 1 + i) & 0xFFFF]
            for nyb in (byte >> 4, byte & 15):
                s = nyb - 16 if nyb >= 8 else nyb
                if shift <= 12:
                    s = (s << shift) >> 1
                else:
                    s = -2048 if s < 0 else 0     # s &= ~0x7FF on the sign-extended value
                if filt >= 8:
                    s += p1
                    s -= p2 >> 1
                    if filt == 8:
                        s += (p2 >> 1) >> 4
                        s += (p1 * -3) >> 6
                    else:
                        s += (p1 * -13) >> 7
                        s += ((p2 >> 1) * 3) >> 4
                elif filt:
                    s += p1 >> 1
                    s += (-p1) >> 5
                s = max(-32768, min(32767, s))
                s = ((s * 2) & 0xFFFF)
                s = s - 0x10000 if s >= 0x8000 else s
                p2, p1 = p1, s
                out.append(s)
        if hdr & 1:                            # end block
            if hdr & 2:
                loop_idx = block_at.get(loop_addr)
                if loop_idx is None:
                    loop_idx = None
            break
        a = (a + 9) & 0xFFFF
    return out, loop_idx


def to8(samples):
    """normalize to int8 with a gain so that value16 ~= pcm8 * gain"""
    peak = max(1, max(abs(x) for x in samples))
    gain = max(1, min(256, (peak + 127) // 127))
    pcm = [max(-128, min(127, int(round(x / gain)))) for x in samples]
    return pcm, gain


def prefilter(samples, loop_idx, a=0.13):
    """approximate the S-DSP gaussian interpolation's lowpass (the player interpolates linearly):
    3-tap [a, 1-2a, a] at the sample's own rate; the loop wraps."""
    n = len(samples)
    out = []
    for i in range(n):
        p = samples[i - 1] if i else 0
        if i + 1 < n:
            q = samples[i + 1]
        else:
            q = samples[loop_idx] if loop_idx is not None else 0
        out.append(max(-32768, min(32767, int(round(a * p + (1 - 2 * a) * samples[i] + a * q)))))
    return out
