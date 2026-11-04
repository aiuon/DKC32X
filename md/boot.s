| DKC 32X level viewer - 68000 boot code (linked at 0x8803F0).
| The first 0x410 bytes are the standard 32X security/startup blob that the
| 32X BIOS expects at 0x3F0 (taken verbatim from marsdev's skeleton).

	.section .text.keepboot

#include "blob.inc"

| 0x880800: exception jump table (the ROM header vectors point here)
_irq_jmptab:
		.word	0x4EF9
		.long	_start			| 0x880800 reset
		.word	0x4EF9
		.long	_except			| 0x880806 exceptions
		.word	0x4EF9
		.long	md_hint			| 0x88080C level 4 (H int)
		.word	0x4EF9
		.long	md_vint			| 0x880812 level 6 (V int)

_except:
		move.l	#0xC0000000,(0xC00004)
		move.w	#0x000E,(0xC00000)
1:		bra.s	1b

_start:
		move.w	#0x2700,%sr
		| clear work RAM
		moveq	#0,%d0
		move.w	#0x3FFF,%d1
		suba.l	%a1,%a1
1:		move.l	%d0,-(%a1)
		dbf		%d1,1b

		| initialised data (and RAM-resident code) to work RAM.  (I22) LZSS-packed by tools/mdpack.py
		| (tools/lzss.py: a flag byte per 8 items, bit 7 first, 1 = literal; a match = 2 bytes
		| (distance - 1) << 4 | (length - 3)) - the matches copy from the output itself (all in RAM)
		lea		__text_end,%a0
		lea		0xFF0000,%a1
		lea		0xFF0000+__data_size,%a2
5:		cmpa.l	%a2,%a1
		bcc.s	9f
		move.b	(%a0)+,%d1				| 8 flags
		moveq	#7,%d2
6:		cmpa.l	%a2,%a1
		bcc.s	9f
		add.b	%d1,%d1
		bcc.s	7f
		move.b	(%a0)+,(%a1)+			| literal
		bra.s	8f
7:		moveq	#0,%d3					| match
		move.b	(%a0)+,%d3
		lsl.w	#8,%d3
		move.b	(%a0)+,%d3
		move.w	%d3,%d4
		andi.w	#15,%d4
		addq.w	#2,%d4					| length - 1 (dbf)
		lsr.w	#4,%d3
		addq.w	#1,%d3					| distance
		movea.l	%a1,%a3
		suba.w	%d3,%a3
2:		move.b	(%a3)+,(%a1)+
		dbf		%d4,2b
8:		dbf		%d2,6b
		bra.s	5b
9:

		| joypad ports: TH as output
		move.b	#0x40,(0xA10009)
		move.b	#0x40,(0xA1000B)
		move.b	#0x40,(0xA10003)

		lea		0x01000000,%sp
		jsr		md_setup

		move.b	#0,(0xA15107)				| clear RV - SH2s may access ROM
3:		cmp.l	#0x4D5F4F4B,(0xA15120)		| "M_OK"
		bne.s	3b
4:		cmp.l	#0x535F4F4B,(0xA15124)		| "S_OK"
		bne.s	4b
		move.w	(0xA15100),%d0
		or.w	#0x8000,%d0
		move.w	%d0,(0xA15100)				| FM: give the SH2s the 32X VDP
		move.l	#0,(0xA15120)				| release the master SH2

		jmp		md_idle
