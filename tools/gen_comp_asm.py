#!/usr/bin/env python3
"""Generates sh/comp.s: the comp-mode (true transparency) line compositor in SH-2 assembly.

comp_band     per-line control loop (row setup callbacks, BG1 ripple, dispatch) over a state block
comp_kK_bB    line without BG1: out = F(base stream) + light, two framebuffer longs per light tile
comp_mix_..   line with BG1: per light tile a class byte picks no-BG1 / opaque-BG1 / partial (merge)
comp_prep     map entries -> {row-pair-0 address, flip xor}; comp_classes: BG1 pair classes

F() is a funnel shift of the base (or BG1) stream by k longs + b bytes (fixed shifts; xtrct for 16).
usage: gen_comp_asm.py out.s [prof]   (prof: FRT timing of each call site + path counters)
"""
import sys
PROF = len(sys.argv) > 2 and sys.argv[2].startswith("prof")
# (an argument "rom": the code in .text, see SECT)
PROF2 = len(sys.argv) > 2 and sys.argv[2] == "prof2"

def funnel(b, x, y):
    if b == 0:
        return [f"mov.l {x},r14"]
    if b == 1:
        return [f"mov.l {x},r14", f"mov.l {y},r0", "shll8 r14", "shlr16 r0", "shlr8 r0", "or r0,r14"]
    if b == 2:
        return [f"mov.l {x},r0", f"mov.l {y},r14", "xtrct r0,r14"]
    return [f"mov.l {x},r14", f"mov.l {y},r0", "shll16 r14", "shll8 r14", "shlr8 r0", "or r0,r14"]

def merge(off, lab, kref):
    c = ["mov r13,r0"]
    if off:
        c.append(f"add #{off},r0")
    c += ["mov.l @(r0,r4),r1", "tst r1,r1", f"bt {lab}",
          f"mov.l {kref},r0", "mov r1,r2", "and r0,r2", "add r0,r2", "or r1,r2", "not r0,r0", "and r0,r2",
          "cmp/eq r0,r2", f"bf {lab}p", f"bra {lab}", "mov r1,r14",
          f"{lab}p:",
          "mov r2,r0", "shlr r0", "shlr2 r0", "shlr2 r0", "shlr2 r0", "add r2,r2", "sub r0,r2",
          "mov r14,r0", "xor r1,r0", "and r2,r0", "xor r0,r14",
          f"{lab}:"]
    return c

def routine(k, b, g):
    name = f"_comp_k{k}_b{b}{'_g' if g else ''}"
    W = ["@r10", "@(4,r10)", "@r11", "@(4,r11)"]
    kref = f"{name}_k7f"
    c = [".align 4", f".global {name}", f"{name}:"]
    c += [f"mov.l r{r},@-r15" for r in range(8, 15)]
    c += ["mov.l @r7+,r8", "mov.l @r7+,r9", "mov.l @r7+,r13", "mov.l @r7,r7",
          "mov.l @r5+,r10", "mov.l @r5+,r0", "xor r8,r0", "add r0,r10"]
    c += [f"{name}_loop:",
          "mov.l @r5+,r11", "mov.l @r5+,r0", "mov.l @r6+,r3", "xor r8,r0", "add r0,r11",
          "mov.l @r6+,r0", "xor r9,r0", "add r0,r3"]
    c += funnel(b, W[k], W[k + 1])
    if g:
        c += merge(0, f"{name}_m0", kref)
    c += ["mov.l @r3,r2", "add r2,r14", "mov.l r14,@r4"]
    c += funnel(b, W[k + 1], W[k + 2])
    if g:
        c += merge(4, f"{name}_m1", kref)
    c += ["mov.l @(4,r3),r2", "add r2,r14", "mov.l r14,@(4,r4)",
          "add #8,r4", "dt r7", f"bf/s {name}_loop", "mov r11,r10"]
    c += [f"mov.l @r15+,r{r}" for r in range(14, 7, -1)]
    c += ["rts", "nop"]
    if g:
        c += [".align 2", f"{kref}:", ".long 0x7F7F7F7F"]
    if PROF2:
        for k in range(3):
            c += [f"{name}_c{k}:", f".long {0xC0000000 + 4 * (300 + 63 + k)}"]
    return c

def funnel_to(b, x, y, dst, tmp):
    if b == 0:
        return [f"mov.l {x},{dst}"]
    if b == 1:
        return [f"mov.l {x},{dst}", f"mov.l {y},{tmp}", f"shll8 {dst}", f"shlr16 {tmp}", f"shlr8 {tmp}", f"or {tmp},{dst}"]
    if b == 2:
        return [f"mov.l {x},{tmp}", f"mov.l {y},{dst}", f"xtrct {tmp},{dst}"]
    return [f"mov.l {x},{dst}", f"mov.l {y},{tmp}", f"shll16 {dst}", f"shll8 {dst}", f"shlr8 {tmp}", f"or {tmp},{dst}"]

def merge_reg(g, lab, kref):
    """r14 = merge(r14, g); temps r0, r3"""
    return [f"tst {g},{g}", f"bt {lab}",
            f"mov.l {kref},r0", f"mov {g},r3", "and r0,r3", "add r0,r3", f"or {g},r3", "not r0,r0", "and r0,r3",
            "cmp/eq r0,r3", f"bf {lab}p", f"bra {lab}", f"mov {g},r14",
            f"{lab}p:",
            "mov r3,r0", "shlr r0", "shlr2 r0", "shlr2 r0", "shlr2 r0", "add r3,r3", "sub r0,r3",
            "mov r14,r0", f"xor {g},r0", "and r3,r0", "xor r0,r14",
            f"{lab}:"]

def pmerge(g, basef, lab, kref):
    """r14 = BG1 long g over the base long (computed by basef only when needed); temps r0, r3"""
    return [f"tst {g},{g}", f"bt {lab}b",
            f"mov.l {kref},r0", f"mov {g},r3", "and r0,r3", "add r0,r3", f"or {g},r3", "not r0,r0", "and r0,r3",
            "cmp/eq r0,r3", f"bf {lab}p", f"bra {lab}", f"mov {g},r14",
            f"{lab}p:"] + basef + [
            "mov r3,r0", "shlr r0", "shlr2 r0", "shlr2 r0", "shlr2 r0", "add r3,r3", "sub r0,r3",
            "mov r14,r0", f"xor {g},r0", "and r3,r0", f"bra {lab}", "xor r0,r14",
            f"{lab}b:"] + basef + [f"{lab}:"]

def mix_routine(kb, bb, kg, bg):
    """void comp_mix_..(volatile u32 *dst, const u32 *bent, const u32 *lent, const u32 *args)
    args: {ob, ol, og, cls (u8 per pair: 0 none, 1 partial, 2 opaque BG1, 3 end), gent}"""
    name = f"_comp_mix_{kb}{bb}_{kg}{bg}"
    WB = ["@r10", "@(4,r10)", "@r11", "@(4,r11)"]
    WG = ["@r1", "@(4,r1)", "@r2", "@(4,r2)"]
    kref = f"{name}_k7f"
    c = [".align 4", f".global {name}", f"{name}:"]
    c += [f"mov.l r{r},@-r15" for r in range(8, 15)]
    # r12 = gent - lent - 8: after the light entry load, BG1 entry j is at r6 + r12
    c += ["mov.l @r7+,r8", "mov.l @r7+,r9", "mov.l @r7+,r13", "mov.l @r7+,r12", "mov.l @r7,r0",
          "mov r12,r7", "sub r6,r0", "add #-8,r0", "mov r0,r12",
          "mov.l @r5+,r10", "mov.l @r5+,r0", "xor r8,r0", "add r0,r10"]
    bg1rows = ["mov r6,r2", "add r12,r2", "mov.l @r2,r1", "mov.l @(4,r2),r0", "xor r13,r0", "add r0,r1",
               "mov.l @(12,r2),r0", "mov.l @(8,r2),r2", "xor r13,r0", "add r0,r2"]
    cnt = lambda k, a, b: ([f"mov.l {name}_c{k},{a}", f"mov.l @{a},{b}", f"add #1,{b}", f"mov.l {b},@{a}"]
                           if PROF2 else [])
    c += [f"{name}_loop:",
          "mov.l @r5+,r11", "mov.l @r5+,r1", "xor r8,r1", "add r1,r11",
          "mov.l @r6+,r3", "mov.l @r6+,r1", "xor r9,r1", "add r1,r3",
          "mov.b @r7+,r0", "tst r0,r0", f"bf {name}_other"] + cnt(0, "r1", "r0")
    # no BG1 in this pair (most common: falls through)
    c += funnel_to(bb, WB[kb], WB[kb + 1], "r14", "r0")
    c += ["mov.l @r3,r0", "add r0,r14", "mov.l r14,@r4"]
    c += funnel_to(bb, WB[kb + 1], WB[kb + 2], "r14", "r0")
    c += ["mov.l @(4,r3),r0", "add r0,r14", "mov.l r14,@(4,r4)"]
    c += [f"{name}_next:", "add #8,r4", f"bra {name}_loop", "mov r11,r10"]
    # opaque BG1 over the whole pair
    c += [f"{name}_other:", "cmp/eq #2,r0", f"bf {name}_o2"] + cnt(1, "r2", "r1") + bg1rows
    c += funnel_to(bg, WG[kg], WG[kg + 1], "r14", "r0")
    c += ["mov.l @r3,r0", "add r0,r14", "mov.l r14,@r4"]
    c += funnel_to(bg, WG[kg + 1], WG[kg + 2], "r14", "r0")
    c += ["mov.l @(4,r3),r0", "add r0,r14", f"bra {name}_next", "mov.l r14,@(4,r4)"]
    c += [f"{name}_o2:", "cmp/eq #3,r0", f"bf {name}_part"]
    c += [f"mov.l @r15+,r{r}" for r in range(14, 7, -1)] + ["rts", "nop"]
    # partial: merge BG1 over the base
    c += [f"{name}_part:", "mov.l r3,@-r15"] + cnt(2, "r3", "r0") + bg1rows
    c += funnel_to(bg, WG[kg], WG[kg + 1], "r3", "r0")
    c += funnel_to(bg, WG[kg + 1], WG[kg + 2], "r14", "r0")
    c += ["mov r14,r2", "mov r3,r1"]
    c += pmerge("r1", funnel_to(bb, WB[kb], WB[kb + 1], "r14", "r0"), f"{name}_m0", kref)
    c += ["mov.l @r15,r3", "mov.l @r3,r0", "add r0,r14", "mov.l r14,@r4"]
    c += pmerge("r2", funnel_to(bb, WB[kb + 1], WB[kb + 2], "r14", "r0"), f"{name}_m1", kref)
    c += ["mov.l @r15+,r3", "mov.l @(4,r3),r0", "add r0,r14", f"bra {name}_next", "mov.l r14,@(4,r4)"]
    c += [".align 2", f"{kref}:", ".long 0x7F7F7F7F"]
    if PROF2:
        for k in range(3):
            c += [f"{name}_c{k}:", f".long {0xC0000000 + 4 * (300 + 63 + k)}"]
    return c

HELPERS = """
! void comp_prep(u32 *ent, const u32 *src, u32 n): ent-form map entries -> {addr, xor (bit0 ? ~0 : 0)}
.align 4
.global _comp_prep
_comp_prep:
	mov.l @r5+,r0
	mov r0,r1
	shlr r1
	subc r1,r1
	mov.l r0,@r4
	mov.l r1,@(4,r4)
	dt r6
	bf/s _comp_prep
	add #8,r4
	rts
	nop

! void comp_classes(u8 *c, const u8 *tc, u32 go_nonzero): 33 pair classes + end marker 3
.align 4
.global _comp_classes
_comp_classes:
	mov #33,r7
	tst r6,r6
	bf _comp_classes_2
_comp_classes_1:
	mov.b @r5+,r0
	dt r7
	mov.b r0,@r4
	bf/s _comp_classes_1
	add #1,r4
	mov #3,r0
	rts
	mov.b r0,@r4
_comp_classes_2:
	mov.b @r5+,r0
	mov.b @r5,r1
	cmp/eq r0,r1
	bt _comp_classes_3
	mov #1,r0
_comp_classes_3:
	dt r7
	mov.b r0,@r4
	bf/s _comp_classes_2
	add #1,r4
	mov #3,r0
	rts
	mov.b r0,@r4
"""

# comp_band: per-line control loop.  State block (u32 fields, see sh/main.c CS_*): r14 = fields 0-15,
# r13 = fields 16-31, r12 = fields 32-47.
def F(i):
    base = "r14" if i < 16 else "r13" if i < 32 else "r12"
    return f"@({(i % 16) * 4},{base})"

# state fields (keep in sync with the CS_* enum in sh/main.c)
CS = dict(ROFF=0, DSTROW=8, SY=9, SYEND=10, OVYB1=11, BH=12, BKEY=13, LKEY=14, WAVE=15,
          LW1=16, GY0=17, GX0C=18, GMROW=19, GTX=20, GANY=21, GCX=22, PAT=23, RPH=24, BX=25,
          MIXTAB=26, CBBASE=27, CBLIGHT=28, CBBG1=29, PLAINTAB=30, TDST=31,
          ARGS=32, PARGS=37, BW1=41, LH=42, FNCLS=43, STRIDE=44, TVAR=45, BOFF=46, LOFF=47)
def G(name, add=0):
    return F(CS[name] + add)

BAND = f"""
! void comp_band(u32 *state): per screen line, light offsets (H, V) from the 4-line band wave table;
! the line is shifted by P = H & 3 (line table/SFT written by the master) so the light is long-aligned
.align 4
.global _comp_band
_comp_band:
	sts.l pr,@-r15
	mov.l r8,@-r15
	mov.l r9,@-r15
	mov.l r10,@-r15
	mov.l r11,@-r15
	mov.l r12,@-r15
	mov.l r13,@-r15
	mov.l r14,@-r15
	mov r4,r14
	mov r4,r13
	add #64,r13
	mov r13,r12
	add #64,r12
_cb_line:
	mov.l {G('SY')},r8
	mov r8,r0
	shlr2 r0
	shll2 r0
	add r0,r0
	mov.l {G('WAVE')},r1
	add r0,r1
	mov.l @r1,r9
	mov.l @(4,r1),r10
	mov r9,r0
	and #3,r0
	mov #32,r11
	sub r0,r11
	mov r9,r1
	add r11,r1
	mov.l {G('LW1')},r2
	and r2,r1
	mov r1,r0
	and #4,r0
	neg r0,r0
	add r0,r11
	mov.l {G('DSTROW')},r2
	add r0,r2
	mov.l r2,{G('TDST')}
	shlr2 r1
	shlr r1
	mov r10,r0
	add r8,r0
	mov.l {G('LH')},r2
	and r2,r0
	mov r0,r10
	shlr2 r0
	shlr r0
	mov.l {G('LKEY')},r2
	mov r2,r3
	shlr8 r3
	cmp/eq r0,r3
	bf _cb_lok_prep
	mov r1,r3
	extu.b r2,r2
	sub r2,r3
	mov.l {G('LW1')},r2
	shlr2 r2
	shlr r2
	and r2,r3
	mov #3,r2
	cmp/hi r2,r3
	bf _cb_lok
_cb_lok_prep:
	mov r0,r2
	shll8 r2
	or r1,r2
	mov.l r2,{G('LKEY')}
	mov r1,r5
	mov.l {G('CBLIGHT')},r1
	jsr @r1
	mov r0,r4
	mov #0,r3
_cb_lok:
	shll2 r3
	add r3,r3
	mov.l r3,{G('LOFF')}
	mov.l {G('OVYB1')},r0
	add r8,r0
	mov.l {G('BH')},r1
	and r1,r0
	mov r0,r9
	mov.l {G('BX')},r1
	add r11,r1
	mov.l {G('BW1')},r2
	and r2,r1
	mov r1,r0
	and #7,r0
	shll2 r0
	mov.l r0,{G('TVAR')}
	shlr2 r1
	shlr r1
	mov r9,r0
	shlr2 r0
	shlr r0
	mov.l {G('BKEY')},r2
	mov r2,r3
	shlr8 r3
	cmp/eq r0,r3
	bf _cb_bok_prep
	mov r1,r3
	extu.b r2,r2
	sub r2,r3
	mov.l {G('BW1')},r2
	shlr2 r2
	shlr r2
	and r2,r3
	mov #3,r2
	cmp/hi r2,r3
	bf _cb_bok
_cb_bok_prep:
	mov r0,r2
	shll8 r2
	or r1,r2
	mov.l r2,{G('BKEY')}
	mov r1,r5
	mov.l {G('CBBASE')},r1
	jsr @r1
	mov r0,r4
	mov #0,r3
_cb_bok:
	shll2 r3
	add r3,r3
	mov.l r3,{G('BOFF')}
	mov r9,r0
	and #7,r0
	shll2 r0
	mov.l @(r0,r14),r1
	mov r10,r0
	and #7,r0
	shll2 r0
	mov.l @(r0,r14),r2
	mov.l r1,{G('ARGS', 0)}
	mov.l r2,{G('ARGS', 1)}
	mov.l r1,{G('PARGS', 0)}
	mov.l r2,{G('PARGS', 1)}
	mov.l {G('RPH')},r0
	mov r8,r1
	sub r0,r1
	mov #63,r0
	and r1,r0
	mov.l {G('PAT')},r1
	mov.b @(r0,r1),r1
	mov.l {G('GY0')},r9
	add r8,r9
	add r1,r9
	mov.l {G('GX0C')},r10
	add r11,r10
	add r1,r10
	mov r9,r0
	shar r0
	shar r0
	shar r0
	mov.l {G('GMROW')},r1
	cmp/eq r0,r1
	bf _cb_gprep
	mov r10,r1
	shar r1
	shar r1
	shar r1
	mov.l {G('GTX')},r2
	sub r2,r1
	mov #1,r2
	cmp/hi r2,r1
	bf _cb_gok
_cb_gprep:
	mov.l r0,{G('GMROW')}
	mov r10,r5
	shar r5
	shar r5
	shar r5
	mov.l r5,{G('GTX')}
	mov #-1,r1
	mov.l r1,{G('GCX')}
	mov.l {G('CBBG1')},r1
	jsr @r1
	mov r0,r4
	mov.l r0,{G('GANY')}
_cb_gok:
	mov.l {G('GANY')},r0
	tst r0,r0
	bt _cb_plain
	mov.l {G('GCX')},r1
	cmp/eq r10,r1
	bt _cb_cls_ok
	mov.l r10,{G('GCX')}
	mov r10,r5
	shar r5
	shar r5
	shar r5
	mov.l {G('GTX')},r1
	sub r1,r5
	mov.l _cb_tcls,r1
	add r1,r5
	mov r10,r0
	and #7,r0
	mov r0,r6
	mov.l _cb_cls,r4
	mov.l {G('FNCLS')},r1
	jsr @r1
	nop
_cb_cls_ok:
	mov r9,r0
	and #7,r0
	shll2 r0
	mov.l @(r0,r14),r1
	mov.l r1,{G('ARGS', 2)}
	mov r10,r1
	shar r1
	shar r1
	shar r1
	mov.l {G('GTX')},r2
	sub r2,r1
	shll2 r1
	add r1,r1
	mov.l _cb_lg,r2
	add r2,r1
	mov.l r1,{G('ARGS', 4)}
	mov.l {G('TVAR')},r0
	shll2 r0
	shll r0
	mov.l {G('MIXTAB')},r1
	add r0,r1
	mov r10,r0
	and #7,r0
	shll2 r0
	mov.l @(r0,r1),r1
	mov.l {G('TDST')},r4
	mov.l _cb_lb,r5
	mov.l {G('BOFF')},r0
	add r0,r5
	mov.l _cb_ll,r6
	mov.l {G('LOFF')},r0
	add r0,r6
	jsr @r1
	mov r12,r7
	bra _cb_next
	nop
_cb_plain:
	mov.l {G('TVAR')},r0
	mov.l {G('PLAINTAB')},r1
	mov.l @(r0,r1),r1
	mov.l {G('TDST')},r4
	mov.l _cb_lb,r5
	mov.l {G('BOFF')},r0
	add r0,r5
	mov.l _cb_ll,r6
	mov.l {G('LOFF')},r0
	add r0,r6
	mov r12,r7
	jsr @r1
	add #20,r7
_cb_next:
	mov.l {G('DSTROW')},r0
	mov.l {G('STRIDE')},r1
	add r1,r0
	mov.l r0,{G('DSTROW')}
	mov.l {G('SY')},r0
	add #1,r0
	mov.l r0,{G('SY')}
	mov.l {G('SYEND')},r1
	cmp/eq r1,r0
	bt _cb_done
	bra _cb_line
	nop
_cb_done:
	mov.l @r15+,r14
	mov.l @r15+,r13
	mov.l @r15+,r12
	mov.l @r15+,r11
	mov.l @r15+,r10
	mov.l @r15+,r9
	mov.l @r15+,r8
	lds.l @r15+,pr
	rts
	nop
.align 2
_cb_lb:
	.long 0xC0000120
_cb_lg:
	.long 0xC0000120 + 304
_cb_ll:
	.long 0xC0000120 + 608
_cb_cls:
	.long 0xC0000000
_cb_tcls:
	.long 0xC0000040
"""


def frt_to_r0():
    return ["mov #-2,r2", "shll8 r2", "add #18,r2", "mov.b @r2+,r0", "extu.b r0,r0", "shll8 r0",
            "mov.b @r2,r2", "extu.b r2,r2", "or r2,r0"]

def prof_band(text):
    """time every callback: accumulators at state fields 57.. (by call site), start time at 56"""
    lines = text.strip("\n").split("\n")
    out, k, i = [], 0, 0
    while i < len(lines):
        l = lines[i]
        if l.strip().startswith("jsr @r1"):
            out += ["\tmov.l r0,@-r15", "\tmov.l r2,@-r15"] + ["\t" + x for x in frt_to_r0()]
            out += ["\tmov r12,r2", "\tadd #96,r2", "\tmov.l r0,@r2", "\tmov.l @r15+,r2", "\tmov.l @r15+,r0"]
            out += [l, lines[i + 1]]
            out += ["\tmov.l r0,@-r15", "\tmov.l r2,@-r15", "\tmov.l r3,@-r15"] + ["\t" + x for x in frt_to_r0()]
            out += ["\tmov r12,r3", "\tadd #96,r3", "\tmov.l @r3,r2", "\tsub r2,r0", "\textu.w r0,r0",
                    f"\tmov.l @({4 + 4 * k},r3),r2", "\tadd r0,r2", f"\tmov.l r2,@({4 + 4 * k},r3)",
                    "\tmov.l @r15+,r3", "\tmov.l @r15+,r2", "\tmov.l @r15+,r0"]
            k += 1
            i += 2
            continue
        out.append(l)
        i += 1
    return out

# (game builds: ROM - their comp levels always use the performance variant, the compositor never runs
#  there; its 25 KB of SDRAM hold hot game code instead.  COMP_SDRAM=1: SDRAM as before)
import os
SECT = '.sdtext' if os.environ.get('COMP_SDRAM', '1' if 'rom' not in sys.argv[2:] else '0') == '1' else '.text'
out = ["! generated by tools/gen_comp_asm.py -- do not edit", '.section %s,"ax"' % SECT]
out += prof_band(BAND) if PROF else BAND.strip("\n").split("\n")
out += HELPERS.strip("\n").split("\n")
for kb in (0, 1):
    for bb in range(4):
        for kg in (0, 1):
            for bg in range(4):
                out += mix_routine(kb, bb, kg, bg)
for k in (0, 1):
    for b in range(4):
        out += routine(k, b, 0)
open(sys.argv[1] if len(sys.argv) > 1 else "sh/comp.s", "w").write(
    "\n".join(l if l.endswith(":") or l.startswith(".") or l.startswith("!") else "\t" + l for l in out) + "\n")
