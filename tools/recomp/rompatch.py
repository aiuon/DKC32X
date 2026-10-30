"""Build-time patches to the game code (applied to the user's ROM image in memory by r65.py and
mkgame.py, so the translated and the interpreted code agree).

WIDESCREEN (env, default 1): the 32X shows SNES x -32..287 (32 columns more on each side, I16).
Only DISPLAY code is widened (the OAM builder's x culls, item drawing).  The game's spawn and
keep-alive windows (camx - $20 .. camx + $120) are game logic and stay as on the SNES (I15) - they
cover exactly the 32X's columns, so nothing pops in inside the picture."""
import os

def _ws():
    p = []
    adc = lambda a, old, new: p.append((a, bytes([0x69, old & 0xFF, old >> 8]), bytes([0x69, new & 0xFF, new >> 8])))
    cmp_ = lambda a, old, new: p.append((a, bytes([0xC9, old & 0xFF, old >> 8]), bytes([0xC9, new & 0xFF, new >> 8])))
    # (I15) The spawn / keep-alive / "acts while on screen" windows below are GAME LOGIC:
    # widening them made the game behave differently from the SNES (snes9x with the same inputs
    # diverged within seconds in Barrel Cannon Canyon; a barrel group lost its moving barrels -
    # no way on).  Off by default (WS_LOGIC=1 restores them, for tests only).
    if os.environ.get('WS_LOGIC', '0') == '1':
        _ws_logic(adc, cmp_, p)
    _ws_display(adc, cmp_, p)
    return p


def _ws_logic(adc, cmp_, p):
    # level sprite spawn / keep-alive windows (camx - $20 + $140), bank BD
    for a in (0x3DF605, 0x3DF710, 0x3DF79D, 0x3DF89C, 0x3DF8E2, 0x3DF9B7, 0x3DF9F5, 0x3DFA3E, 0x3DFAE8,
              0x3DFB1E, 0x3DFCD9, 0x3DFD0D, 0x3DFF37, 0x3DFFBE):
        adc(a, 0x140, 0x180)
    adc(0x3DFA0A, 0x128, 0x168)     # "already visible" inner window (camx - $14 + $128)
    adc(0x3DFA21, 0x1A8, 0x1E8)     # wide window (camx - $54 + $1A8)
    # (I11) the two other spawn edges (CODE_BDF585 / CODE_BDFB76: camx + $120 -> objects appeared
    # 32 px left of the 32X's right edge)
    adc(0x3DF595, 0x120, 0x160)
    adc(0x3DFB8E, 0x120, 0x160)
    # (I11) enemies that act only while on screen (CODE_BED58F: rel $10 .. $EF)
    cmp_(0x3ED5B2, 0xF0, 0x130)


def _ws_display(adc, cmp_, p):
    """(I16) the 32X view is SNES x -32 .. 287 (centred, main.c WS_CENTER): only DISPLAY code changes.
    The game spawns objects in camx - $20 .. camx + $120 - the same columns - so its logic stays as is
    (falling leaves / snow flakes wrap in that range too: unpatched)."""
    # OAM builder x culls (bank BB): rel + $30 < $160, rel + $58 < $1B0 -> rel -112 .. 367 (both sides;
    # the view also moves up to 64 px left of the game's camera at a level's right end)
    if os.environ.get('WS_LEFT', '1') == '1':
        adc(0x3BA8DD, 0x30, 0x70)
        cmp_(0x3BA8E0, 0x160, 0x1E0)
        adc(0x3BA90D, 0x58, 0x98)
        cmp_(0x3BA910, 0x1B0, 0x230)
    else:
        cmp_(0x3BA8E0, 0x160, 0x1A0)
        cmp_(0x3BA910, 0x1B0, 0x1F0)
    # item drawing (bananas, banana bunches in the item table, CODE_B8B918 / B8B9B5): drawn up to
    # $BE + $100 (right edge, $5E) / $107 (visible columns); the OAM x bit 8 is set only for x < 0
    # (XBA ASL: carry = the sign) - now x bit 8 itself (XBA LSR), so x 256..319 lands right
    if os.environ.get('WS_ITEMS', '1') != '1':
        return
    adc(0x38B91B, 0x100, 0x140)     # $5E = $BE + $140
    adc(0x38B942, 0x10F, 0x16F)     # left edge (x - $5E + w + this < 0: skip): items down to x - 47
    adc(0x38B9EA, 0x00F, 0x02F)     # (I16) columns clipped on the left from x < -47 (was -15)
    p.append((0x38BA11, bytes([0xA9, 0x07, 0x01]), bytes([0xA9, 0x47, 0x01])))   # LDA #$0107 -> $0147
    for a in (0x38BA90, 0x38BAF3):
        p.append((a, bytes([0xEB, 0x0A]), bytes([0xEB, 0x4A])))                   # XBA ASL -> XBA LSR


GFX_LISTS, GFX_NLISTS = 0x39A994, 30     # DATA_B9A994: offsets of the graphics lists (CODE_B9A924)


def _gfxskip(d):
    """GFXSKIP (env, default 1): the level graphics lists (CODE_B9A924, 7-byte entries: bank, src,
    VRAM dest (bit 15 = compressed), size) decompress BG graphics the 32X never shows (it draws its
    own converted tiles).  Compressed entries with a BG destination (word $2000+, except the cabin
    text area $7600-$77FF) become a 2-byte DMA from $80:8000 to the same address: no decompression,
    so their compressed data is not read (mkgame.py leaves those files out).  -> patched sources."""
    src = []
    if os.environ.get('GFXSKIP', '1') != '1':
        return src
    for i in range(GFX_NLISTS):
        x = d[GFX_LISTS + 2 * i] | d[GFX_LISTS + 2 * i + 1] << 8
        while True:
            e = GFX_LISTS + x
            bank = d[e]
            if not bank:
                break
            dest = d[e + 3] | d[e + 4] << 8
            if dest & 0x8000:
                w = dest & 0x7FFF
                n = d[e + 5] | d[e + 6] << 8
                if w >= 0x2000 and not (w < 0x7800 and w + (n >> 1) > 0x7600):
                    src.append((bank << 16 | d[e + 1] | d[e + 2] << 8) & 0xFFFFFF)
                    d[e:e + 7] = bytes([0x80, 0x00, 0x80, w & 0xFF, w >> 8, 0x02, 0x00])
            x += 7
    # CODE_B98CEF (temple levels) decompresses DATA_E73BDE (BG1 graphics) itself before list $0E
    # uploads the buffer: JSL CODE_B896FC -> 4 x NOP (the upload then sends stale buffer bytes to BG
    # VRAM, which the 32X never shows)
    a, old = 0x398D0C, bytes([0x22, 0xFC, 0x96, 0xB8])
    if bytes(d[a:a + 4]) == old:
        d[a:a + 4] = bytes([0xEA] * 4)
        src.append(0xE73BDE)
    return src


def gfxskip_sources(rom):
    """the compressed BG graphics no longer read (SNES addresses of their data)"""
    return _gfxskip(bytearray(rom))


def _palfix(d):
    """(I21d) PALFIX (env, default 1): the sprite palette allocator (CODE_BDF1F4) with all 8 OBJ palette
    slots in use returns slot 0 WITHOUT counting the new user (CODE_BDF22B), but that object frees slot 0
    when it goes ($1AE3 - 1): two such objects and slot 0 (the bananas' palette) is released while the
    bananas still use it - the next palette loaded there (a Krusha's) recoloured the bananas and every
    overflowed object (Manic Mincers, OBJECT RANGE WIDE: more objects alive, more palettes).  The new
    user is counted now (INC $1AE3 in place of the debug display's LDA $8C / STA $1E35)."""
    if os.environ.get('PALFIX', '1') != '1':
        return
    a, old, new = 0x3DF231, bytes([0xA5, 0x8C, 0x8D, 0x35, 0x1E]), bytes([0xEE, 0xE3, 0x1A, 0xEA, 0xEA])
    assert bytes(d[a:a + 5]) == old, 'rompatch: unexpected bytes at %06X' % a
    d[a:a + 5] = new


def dis_fix(insns):
    """the disassembly's instruction boundaries (disparse: pc -> (mnemonic, operand bytes, ..)) for the
    patched code, so r65.py's static discovery follows it (untraced code must match the disassembly)"""
    if os.environ.get('PALFIX', '1') != '1':
        return
    insns.pop(0xBDF233, None)
    insns[0xBDF231] = ('INC', 2, 0, 'INC')
    insns[0xBDF234] = ('NOP', 0, 0, 'NOP')
    insns[0xBDF235] = ('NOP', 0, 0, 'NOP')


def apply(rom):
    d = bytearray(rom)
    _gfxskip(d)
    _palfix(d)
    if os.environ.get('WIDESCREEN', '1') == '0':
        return bytes(d)
    for a, old, new in _ws():
        assert bytes(d[a:a + len(old)]) == old, 'rompatch: unexpected bytes at %06X' % a
        d[a:a + len(new)] = new
    return bytes(d)


# (I21) OBJECT RANGE option (pause menu): the game's horizontal sprite spawn / keep-alive windows,
# camx - $20 .. camx + $120 on the SNES (its view + 32 px each side), widened at RUN TIME by
# r65_ws[] (main.c: 0 = SNES; WIDE = the 32X view + 32 px each side, following the view offset d).
# Translated code reads (imm + r65_ws[kind]) at these PCs, the interpreter adds the same (r65_ws_tab).
# kind 0: the window's left edge (SBC: lo = camx - imm - L), kind 1: its width (ADC: hi = lo + imm +
# L + R), kind 2: the right edge from camx (ADC camx + imm + R; "acts while on screen" CMP rel < imm + R)
WS_TUNE = {}
for _a in (0xBDF5AE, 0xBDF5FC, 0xBDF705, 0xBDF757, 0xBDF792, 0xBDF88E, 0xBDF8D9, 0xBDF9A6, 0xBDF9EC,
           0xBDFA35, 0xBDFADF, 0xBDFB11, 0xBDFBA9, 0xBDFCD0, 0xBDFD04, 0xBDFE95, 0xBDFF29, 0xBDFFB1,
           0xBDFA01, 0xBDFA18, 0xB69A60):
    WS_TUNE[_a] = 0
for _a in (0xBDF605, 0xBDF710, 0xBDF79D, 0xBDF89C, 0xBDF8E2, 0xBDF9B7, 0xBDF9F5, 0xBDFA3E, 0xBDFAE8,
           0xBDFB1E, 0xBDFCD9, 0xBDFD0D, 0xBDFF37, 0xBDFFBE, 0xBDFA0A, 0xBDFA21, 0xB69A69):
    WS_TUNE[_a] = 1
for _a in (0xBDF595, 0xBDFB8E, 0xBED5B2):
    WS_TUNE[_a] = 2
