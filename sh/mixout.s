! Output stage of sh/spcdsp.c on the SH-2: echo (delay line with a 2-tap FIR and feedback, run at
! half the output rate), one-pole lowpass (k = 7/8) and conversion to PWM pulse widths.
!
! void mixout(const int32_t *acc, const int32_t *ein, uint32_t *out, mixout_t *st)
!   acc: n frames {L, R}, master volume included (amp16 = x >> 13); ein: the echo-enabled part of
!   acc.  n is even.  out: n longs (L pulse << 16 | R pulse)
! mixout_t (offsets):
!   0 n        4 kinL      8 kinR      12 evl<<18  16 evr<<18  20 efb<<18
!  24 c0       28 c1       32 ebase    36 epos     40 eend     44 flags (1 echo, 2 write)
!  48 pmax     52 lpl      56 lpr      60 center-4096
!   kin = 2^25 / mvol: (ein[a] + ein[b]) * kin >> 32 = mean echo input without master volume, 16-bit
! The echo line holds 8-bit {L, R} pairs; its length includes the FIR delay, so the two taps are the
! two oldest entries: tap0 = epos (overwritten this step), tap1 = the next one.

	.section .sdtext,"ax"
	.align 4
	.global _mixout
_mixout:
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov.l	r10,@-r15
	mov.l	r11,@-r15
	mov.l	r12,@-r15
	mov.l	r13,@-r15
	mov.l	r14,@-r15
	sts.l	pr,@-r15
	mov.l	@r7,r8
	shlr	r8			! frame pairs
	mov.l	@(36,r7),r9		! epos
	mov.l	@(40,r7),r10		! eend
	mov.l	@(32,r7),r12		! ebase
	mov.l	@(24,r7),r13		! c0
	mov.l	@(28,r7),r14		! c1
	mov.l	@(52,r7),r11		! lpl
	mov.l	@(44,r7),r0
	tst	#1,r0
	bt	.Lnoecho

.Lecho:
	mov	r9,r3			! r3 = next entry (tap1)
	add	#2,r3
	cmp/hs	r10,r3
	bf	1f
	mov	r12,r3
1:
	! ---- left: FIR sum (16-bit scale) -> echo out (pushed), feedback + input -> line
	mov.b	@r9,r1
	mov.b	@r3,r2
	muls.w	r1,r13
	sts	macl,r1
	muls.w	r2,r14
	mov.l	@(12,r7),r0
	sts	macl,r2
	add	r2,r1
	shll8	r1
	dmuls.l	r1,r0
	mov.l	@(20,r7),r0
	sts	mach,r2			! echo out L (16-bit)
	dmuls.l	r1,r0
	mov.l	r2,@-r15
	mov.l	@r5,r1			! echo input L, two frames
	mov.l	@(8,r5),r0
	add	r0,r1
	sts	mach,r2			! feedback
	mov.l	@(4,r7),r0
	dmuls.l	r1,r0
	mov.l	@(44,r7),r0
	sts	mach,r1
	add	r1,r2
	tst	#2,r0
	bt	2f
	bsr	.Lclampw
	nop
	mov.b	r0,@r9
2:
	! ---- right
	mov.b	@(1,r9),r0
	mov	r0,r1
	mov.b	@(1,r3),r0
	muls.w	r1,r13
	sts	macl,r1
	muls.w	r0,r14
	mov.l	@(16,r7),r0
	sts	macl,r2
	add	r2,r1
	shll8	r1
	dmuls.l	r1,r0
	mov.l	@(20,r7),r0
	sts	mach,r2			! echo out R
	dmuls.l	r1,r0
	mov.l	r2,@-r15
	mov.l	@(4,r5),r1
	mov.l	@(12,r5),r0
	add	r0,r1
	sts	mach,r2
	mov.l	@(8,r7),r0
	dmuls.l	r1,r0
	mov.l	@(44,r7),r0
	sts	mach,r1
	add	r1,r2
	tst	#2,r0
	bt	3f
	bsr	.Lclampw
	nop
	mov.b	r0,@(1,r9)
3:
	add	#16,r5
	mov	r3,r9
	! ---- two output frames, echo out (16-bit) << 13 added
	mov.l	@r15+,r2		! echo R
	mov.l	@r15+,r1		! echo L
	shll8	r1
	shll8	r2
	shll2	r1
	shll2	r2
	shll2	r1
	shll2	r2
	shll	r1
	shll	r2
	mov.l	r1,@-r15
	mov.l	r2,@-r15
	mov.l	@r4+,r0
	add	r0,r1
	mov.l	@r4+,r0
	bsr	.Lpwm
	add	r0,r2
	mov.l	@r15+,r2
	mov.l	@r15+,r1
	mov.l	@r4+,r0
	add	r0,r1
	mov.l	@r4+,r0
	bsr	.Lpwm
	add	r0,r2
	dt	r8
	bf	.Lecho
	bra	.Ldone
	nop

.Lnoecho:
	mov.l	@r4+,r1
	bsr	.Lpwm
	mov.l	@r4+,r2
	mov.l	@r4+,r1
	bsr	.Lpwm
	mov.l	@r4+,r2
	dt	r8
	bf	.Lnoecho

.Ldone:
	mov.l	r9,@(36,r7)
	mov.l	r11,@(52,r7)
	lds.l	@r15+,pr
	mov.l	@r15+,r14
	mov.l	@r15+,r13
	mov.l	@r15+,r12
	mov.l	@r15+,r11
	mov.l	@r15+,r10
	mov.l	@r15+,r9
	rts
	mov.l	@r15+,r8

! r0 = low byte of (clamp(r2, -32768, 0x7F7F) + 0x80) >> 8
.Lclampw:
	mov.w	.Lwmax,r0
	cmp/gt	r0,r2
	bf	4f
	mov	r0,r2
4:
	mov.w	.Lwmin,r0
	cmp/ge	r0,r2
	bt	5f
	mov	r0,r2
5:
	add	#127,r2
	add	#1,r2
	shlr8	r2
	rts
	mov	r2,r0

! r1 = L, r2 = R (amp16 << 13) -> lowpass (lp += d - d/8) -> PWM long to @r6+ ; uses r0, r3
.Lpwm:
	sub	r11,r1			! d = x - lpl
	mov	r1,r0
	shar	r0
	shar	r0
	shar	r0
	sub	r0,r1
	add	r1,r11			! lpl += d - d/8
	mov.l	@(56,r7),r3
	sub	r3,r2
	mov	r2,r0
	shar	r0
	shar	r0
	shar	r0
	sub	r0,r2
	add	r2,r3
	mov.l	r3,@(56,r7)		! lpr
	! pulse = ((x ^ 0x80000000) >> 19) + center - 4096, then clamped to 1..pmax
	mov.l	.Lbias,r0
	mov	r11,r1
	xor	r0,r1
	xor	r0,r3
	shlr16	r1
	shlr16	r3
	shlr2	r1
	shlr2	r3
	shlr	r1
	shlr	r3
	mov.l	@(60,r7),r0
	add	r0,r1
	add	r0,r3
	mov.l	@(48,r7),r0
	cmp/gt	r0,r1
	bf	6f
	mov	r0,r1
6:
	cmp/gt	r0,r3
	bf	7f
	mov	r0,r3
7:
	cmp/pl	r1
	bt	8f
	mov	#1,r1
8:
	cmp/pl	r3
	bt	9f
	mov	#1,r3
9:
	shll16	r1
	or	r3,r1
	mov.l	r1,@r6
	rts
	add	#4,r6

	.align 2
.Lbias:	.long	0x80000000
.Lwmax:	.word	0x7F7F
.Lwmin:	.word	-32768
