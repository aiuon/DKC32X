! DKC 32X level viewer - ROM header, MARS header and SH-2 startup.
! Based on Chilly Willy's 32X startup code as distributed with marsdev (MIT).
! Must be the first object linked.

	.text
	.org 0

		.long	0x01000000
	.rept	63
		.long	0x000003F0
	.endr

		.ascii	"SEGA 32X        "
		.ascii	"(C)HOMEBREW2026 "
		.ascii	"DONKEY KONG COUNTRY 32X (PERSONAL BUILD)        "
		.ascii	"DONKEY KONG COUNTRY 32X (PERSONAL BUILD)        "
		.ascii	"GM 00000000-00  "
		.ascii	"J6              "
		.long	0x000000
		.long	0x3FFFFF
		.long	0xFF0000
		.long	0xFFFFFF
		.ascii	"RA"			/* battery SRAM: 8-bit on odd bytes, $200001-$201FFF (4 KB) */
		.byte	0xF8, 0x20
		.long	0x00200001
		.long	0x00201FFF
		.ascii	"            "
		.ascii	"                                        "
		.ascii	"JUE             "

/* M68000 jmp (addr).l / jsr (addr).l */
.macro	md_jmp	_addr
	.word	0x4EF9,\_addr>>16,\_addr&0xFFFF
.endm
.macro	md_jsr	_addr
	.word	0x4EB9,\_addr>>16,\_addr&0xFFFF
.endm

		md_jmp	0x880800	/* reset = hot start */
		md_jsr	0x880806	/* EX_BusError */
		md_jsr	0x880806	/* EX_AddrError */
		md_jsr	0x880806	/* EX_IllInstr */
		md_jsr	0x880806	/* EX_DivByZero */
		md_jsr	0x880806	/* EX_CHK */
		md_jsr	0x880806	/* EX_TrapV */
		md_jsr	0x880806	/* EX_Priviledge */
		md_jsr	0x880806	/* EX_Trace */
		md_jsr	0x880806	/* EX_LineA */
		md_jsr	0x880806	/* EX_LineF */
		.space	72			/* reserved */
		md_jsr	0x880806	/* EX_Spurious */
		md_jsr	0x880806	/* EX_Level1 */
		md_jsr	0x880806	/* EX_Level2 */
		md_jsr	0x880806	/* EX_Level3 */
		md_jmp	0x88080C	/* EX_Level4 HBlank */
		md_jsr	0x880806	/* EX_Level5 */
		md_jmp	0x880812	/* EX_Level6 VBlank */
		md_jsr	0x880806	/* EX_Level7 */
	.rept 16
		md_jsr	0x880806	/* traps */
	.endr
		.space	166			/* reserved */

MARS_HEADER:
		.ascii	"DKC32X VIEWER   "	/* module name (16 chars) */
		.long	0x00000000		/* version */
		.long	_stext			/* Source (in ROM) */
		.long	0x00000000		/* Destination (in SDRAM) */
		.long	_sdata			/* Size */
		.long	0x06000240		/* Master SH2 Jump */
		.long	0x06000244		/* Slave SH2 Jump */
		.long	0x06000000		/* Master SH2 VBR */
		.long	0x06000120		/* Slave SH2 VBR */

! 68000 program at 0x3F0 (security blob + our code)
M68K_CODE:
		.incbin "build/md.bin"

	.data

! Master Vector Base Table at 0x06000000
		.long   mstart      /* Cold Start PC */
		.long   0x0603F7F0  /* Cold Start SP (master stack 0603E800-F7F0, canaries at E800 and F7F0) */
		.long   mstart      /* Manual Reset PC */
		.long   0x0603F7F0  /* Manual Reset SP */
		.long   main_err    /* Illegal instruction */
		.long   0x00000000  /* reserved */
		.long   main_err    /* Invalid slot instruction */
		.long   0x20100400  /* reserved */
		.long   0x20100420  /* reserved */
		.long   main_err    /* CPU address error */
		.long   main_err    /* DMA address error */
		.long   main_err    /* NMI vector */
		.long   main_err    /* User break vector */
		.space  76          /* reserved */
	.rept 32
		.long   main_err    /* TRAPA #32..#63 */
	.endr
		.long   other_irq   /* Level 1 IRQ */
		.long   other_irq   /* Level 2 & 3 IRQ's */
		.long   other_irq   /* Level 4 & 5 IRQ's */
		.long   other_irq   /* PWM interupt */
		.long   other_irq   /* Command interupt */
		.long   _master_hint /* H Blank interupt (C handler) */
		.long   mv_stub     /* V Blank interupt (C handler, via the PC recorder) */
		.long   m_vres      /* Reset Button (I17: restart, ROM code) */

! Slave Vector Base Table at 0x06000120
		.long   sstart      /* Cold Start PC */
		.long   0x06040000  /* Cold Start SP */
		.long   sstart      /* Manual Reset PC */
		.long   0x06040000  /* Manual Reset SP */
		.long   main_err    /* Illegal instruction */
		.long   0x00000000  /* reserved */
		.long   main_err    /* Invalid slot instruction */
		.long   0x20100400  /* reserved */
		.long   0x20100420  /* reserved */
		.long   main_err    /* CPU address error */
		.long   main_err    /* DMA address error */
		.long   main_err    /* NMI vector */
		.long   main_err    /* User break vector */
		.space  76          /* reserved */
	.rept 31
		.long   main_err
	.endr
		.long   s_dma1_irq  /* vector 63: DMA channel 1 end (music, VCRDMA1 = 63) */
	.rept 5
		.long   other_irq   /* level 1-5 IRQ, PWM, command */
	.endr
		.long   _slave_hint /* H Blank interupt (C handler): the sky colour per line */
		.long   sv_stub     /* V Blank interupt (C handler, via the PC recorder) */
		.long   s_vres      /* Reset Button (I17: restart, ROM code) */

! The main SH2 starts here at 0x06000240
mstart:
		bra     mcont
		nop
! The slave SH2 starts here at 0x06000244
sstart:
		bra     scont
		nop

mcont:
		mov.l   _int_clr,r1
		mov.w   r0,@-r1     /* PWM INT clear */
		mov.w   r0,@r1
		mov.w   r0,@-r1     /* CMD INT clear */
		mov.w   r0,@r1
		mov.w   r0,@-r1     /* H INT clear */
		mov.w   r0,@r1
		mov.w   r0,@-r1     /* V INT clear */
		mov.w   r0,@r1
		mov.w   r0,@-r1     /* VRES INT clear */
		mov.w   r0,@r1

		mov.l   _master_stk,r15
		mov.l   _cctl,r0    /* purge cache and turn it off */
		mov     #0x10,r1
		mov.b   r1,@r0

		mov     #0,r0       /* clear bss */
		mov.l   _bss_start,r1
		mov.l   _bss_end,r2
	0:
		mov.l   r0,@r1
		cmp/eq  r1,r2
		bf/s    0b
		add     #4,r1

		mov.l   _master_sts,r0  /* wait for 68000 to finish init */
		mov.l   _master_ok,r1
	1:
		mov.l   @r0,r2
		nop
		nop
		cmp/eq  r1,r2
		bt      1b

		mov     #0,r1           /* let slave run */
		mov.l   r1,@(4,r0)

		mov     #0x80,r0
		mov.l   _adapter,r1
		mov.b   r0,@r1          /* set FM */
		mov     #0x00,r0
		mov.b   r0,@(1,r1)      /* int enables (main sets them later) */
		mov     #0x20,r0
		ldc     r0,sr           /* allow ints */

		mov.l   _cctl,r0        /* purge cache, turn it on */
		mov     #0x11,r1
		mov.b   r1,@r0
		mov.l   _master_go,r0
		jsr     @r0
		nop
	2:
		bra     2b
		nop

scont:
		mov.l   _int_clr,r1
		mov.w   r0,@-r1
		mov.w   r0,@r1
		mov.w   r0,@-r1
		mov.w   r0,@r1
		mov.w   r0,@-r1
		mov.w   r0,@r1
		mov.w   r0,@-r1
		mov.w   r0,@r1
		mov.w   r0,@-r1
		mov.w   r0,@r1

		mov.l   _slave_stk,r15
		mov.l   _slave_sts,r0   /* wait for master and 68000 */
		mov.l   _slave_ok,r1
	1:
		mov.l   @r0,r2
		nop
		nop
		cmp/eq  r1,r2
		bt      1b

		mov.l   _adapter,r1
		mov     #0x00,r0
		mov.b   r0,@(1,r1)
		mov     #0x20,r0
		ldc     r0,sr

		mov.l   _cctl,r0
		mov     #0x11,r1
		mov.b   r1,@r0
		mov.l   _slave_go,r0
		jmp     @r0
		nop

		.align   2
_int_clr:
		.long   0x2000401E
_master_stk:
		.long   0x0603F7F0
_slave_stk:
		.long   0x06040000
_master_sts:
		.long   0x20004020
_master_ok:
		.ascii  "M_OK"
_slave_sts:
		.long   0x20004024
_slave_ok:
		.ascii  "S_OK"
_adapter:
		.long   0x20004000
_cctl:
		.long   0xFFFFFE92
_master_go:
		.long   _m_main
_slave_go:
		.long   _s_main
_bss_start:
		.long   __bss_start
_bss_end:
		.long   __end

! V interrupt stubs: record the interrupted PC (hang watchdog page), then the C handler (which
! returns with rte; the stack is as at entry again when it starts)
mv_stub:
		mov.l   r0,@-r15
		mov.l   r1,@-r15
		mov.l   @(8,r15),r0
		mov.l   _mv_pc,r1
		mov.l   r0,@r1
		mov.l   _mv_c,r0
		mov.l   @r15+,r1
		jmp     @r0
		mov.l   @r15+,r0
sv_stub:
		mov.l   r0,@-r15
		mov.l   r1,@-r15
		mov.l   @(8,r15),r0
		mov.l   _sv_pc,r1
		mov.l   r0,@r1
		mov.l   _sv_c,r0
		mov.l   @r15+,r1
		jmp     @r0
		mov.l   @r15+,r0
		.align  2
_mv_pc:
		.long   _m_irq_pc
_mv_c:
		.long   _master_vint
_sv_pc:
		.long   _s_irq_pc + 0x20000000
_sv_c:
		.long   _slave_vint

main_err:
		mov.l   r0,@-r15
		mov.l   r1,@-r15
		mov.l   _err_rec,r0
		mov.l   @(8,r15),r1     /* faulting PC */
		mov.l   r1,@r0
		mov.l   @(12,r15),r1    /* SR */
		mov.l   r1,@(4,r0)
		mov.l   @(8,r0),r1
		tst     r1,r1
		bf      1f
		sts     pr,r1
		mov.l   r1,@(12,r0)     /* PR at the first exception */
		mov     r15,r1
		mov.l   r1,@(16,r0)     /* SP */
		mov.l   @(8,r0),r1
	1:
		add     #1,r1
		mov.l   r1,@(8,r0)      /* count */
		mov.l   @r15+,r1
		mov.l   @r15+,r0
		rte
		nop
		.align  2
_err_rec:
		.long   0x2603F3E0

! Slave DMA channel 1 end: music refill (C), saving everything C may clobber.
s_dma1_irq:
		sts.l   pr,@-r15
		mov.l   r0,@-r15
		mov.l   r1,@-r15
		mov.l   r2,@-r15
		mov.l   r3,@-r15
		mov.l   r4,@-r15
		mov.l   r5,@-r15
		mov.l   r6,@-r15
		mov.l   r7,@-r15
		sts.l   mach,@-r15
		sts.l   macl,@-r15
		mov.l   _music_irq_p,r0
		jsr     @r0
		nop
		lds.l   @r15+,macl
		lds.l   @r15+,mach
		mov.l   @r15+,r7
		mov.l   @r15+,r6
		mov.l   @r15+,r5
		mov.l   @r15+,r4
		mov.l   @r15+,r3
		mov.l   @r15+,r2
		mov.l   @r15+,r1
		mov.l   @r15+,r0
		lds.l   @r15+,pr
		rte
		nop
		.align  2
_music_irq_p:
		.long   _music_irq

! Any unexpected IRQ: clear every 32X interrupt source and return.
other_irq:
		mov.l   r0,@-r15
		mov.l   r1,@-r15
		mov.l   _adapter2,r1
		mov.w   r0,@(0x1C,r1)
		mov.w   r0,@(0x1A,r1)
		mov.w   r0,@(0x14,r1)
		mov.w   @(0x1C,r1),r0   /* read back so the clear lands before rte */
		mov.l   @r15+,r1
		mov.l   @r15+,r0
		rte
		nop
		.align  2
_adapter2:
		.long   0x20004000

	.text
		.global _start
_start:

! (I17) The console's reset button (VRES, level 14 on both SH-2s; the 68000 starts over at its hot
! start, which waits for M_OK / S_OK like at power-on).  Before, the SH-2s went on with the game while
! the 68000 waited for that handshake: a STUCK page.  Now both restart as at power-on: the slave stops
! its DMA and parks in this ROM code, the master copies the SDRAM image (.data: vectors, start code,
! initial data, SDRAM code) from ROM again, then both post the boot handshake and run the cold start.
! (MiSTer / ares do not raise VRES - their reset is a power-on; PicoDrive and the real console do.)
		.align  2
m_vres:
		mov.l   _vr_adapter,r1
		mov.w   r0,@(0x14,r1)   /* VRES clear */
		mov     #0x0F,r0
		shll2   r0
		shll2   r0
		ldc     r0,sr           /* no interrupts */
		mov.l   _vr_mstk,r15
		bsr     vr_stop
		nop
		mov     #0,r0
		mov.l   _vr_comm4,r1
		mov.l   r0,@r1          /* frame / command (a command in flight is void) */
		mov.l   r0,@(4,r1)      /* pad / HUD */
		mov.l   _vr_comm12,r1   /* the slave parked? */
		mov.l   _vr_srdy,r2
	1:
		mov.l   @r1,r0
		cmp/eq  r2,r0
		bf      1b
		mov.l   _vr_src,r1      /* the SDRAM image again (cache-through both ways) */
		mov.l   _vr_dst,r2
		mov.l   _vr_len,r3
		shlr2   r3
	2:
		mov.l   @r1+,r0
		mov.l   r0,@r2
		dt      r3
		bf/s    2b
		add     #4,r2
		mov.l   _vr_comm12,r1
		mov.l   _vr_mdon,r0
		mov.l   r0,@r1          /* the slave may go */
		mov.l   _vr_comm0,r1
		mov.l   _vr_mok,r0
		mov.l   r0,@r1          /* boot handshake for the 68000 */
		mov.l   _vr_mstart,r0
		jmp     @r0
		nop

s_vres:
		mov.l   _vr_adapter,r1
		mov.w   r0,@(0x14,r1)
		mov     #0x0F,r0
		shll2   r0
		shll2   r0
		ldc     r0,sr
		mov.l   _vr_sstk,r15
		bsr     vr_stop
		nop
		mov.l   _vr_comm12,r1
		mov.l   _vr_srdy,r0
		mov.l   r0,@r1          /* parked (ROM code from here) */
		mov.l   _vr_mdon,r2
	1:
		mov.l   @r1,r0
		cmp/eq  r2,r0
		bf      1b
		mov     #0,r0
		mov.l   r0,@r1          /* job token / done = 0 as at power-on */
		mov.l   _vr_comm4,r1
		mov.l   _vr_sok,r0
		mov.l   r0,@r1
		mov.l   _vr_sstart,r0
		jmp     @r0
		nop

! this CPU's DMA (the slave's music) off, its cache purged and off
vr_stop:
		mov     #0,r0
		mov.l   _vr_dmaor,r1
		mov.l   r0,@r1
		mov.l   _vr_chcr0,r1
		mov.l   r0,@r1
		mov.l   _vr_chcr1,r1
		mov.l   r0,@r1
		mov.l   _vr_ccr,r1
		mov     #0x10,r0
		rts
		mov.b   r0,@r1

		.align  2
_vr_adapter:	.long   0x20004000
_vr_mstk:	.long   0x0603F7F0
_vr_sstk:	.long   0x06040000
_vr_comm0:	.long   0x20004020
_vr_comm4:	.long   0x20004024
_vr_comm12:	.long   0x2000402C
_vr_srdy:	.ascii  "SRDY"
_vr_mdon:	.ascii  "MDON"
_vr_mok:	.ascii  "M_OK"
_vr_sok:	.ascii  "S_OK"
_vr_src:	.long   0x22000000 + _stext
_vr_dst:	.long   0x26000000
_vr_len:	.long   _sdata
_vr_mstart:	.long   0x06000240
_vr_sstart:	.long   0x06000244
_vr_dmaor:	.long   0xFFFFFFB0
_vr_chcr0:	.long   0xFFFFFF8C
_vr_chcr1:	.long   0xFFFFFF9C
_vr_ccr:	.long   0xFFFFFE92
