! Voice mixing inner loop for sh/spcdsp.c (SH-2).
!
! uint32_t mixv(int32_t *acc, const int16_t *pcm, uint32_t pos, mixv_args_t *a)
!   a = { step, n, gl, gr, end, looplen, dgl, dgr } (16.16 positions; gains 14-bit signed, ramped
!   by dgl/dgr per frame and written back; pcm 15-bit)
!   acc[2*j] += s*gl, acc[2*j+1] += s*gr for n frames, s = linear interpolation at pos.
!   returns the new pos, or 0xFFFFFFFF when a non-looping sample ended.

	.section .sdtext,"ax"
	.align 4
	.global _mixv
_mixv:
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov.l	r10,@-r15
	mov.l	r11,@-r15
	mov.l	r12,@-r15
	mov.l	r13,@-r15
	mov.l	r14,@-r15
	mov.l	r7,@-r15
	mov.l	@(24,r7),r14		! dgl
	mov.l	@r7,r8			! step
	mov.l	@(4,r7),r9		! n
	mov.l	@(8,r7),r10		! gl
	mov.l	@(12,r7),r11		! gr
	mov.l	@(16,r7),r12		! end
	mov.l	@(20,r7),r13		! looplen
	mov.l	@(28,r7),r7		! dgr
.Lloop:
	mov	r6,r0
	shlr16	r0
	add	r0,r0
	mov.w	@(r0,r5),r1		! s0
	add	#2,r0
	mov.w	@(r0,r5),r2		! s1
	extu.w	r6,r3
	shlr	r3			! frac, 15 bits
	sub	r1,r2			! d = s1 - s0
	muls.w	r2,r3
	add	r8,r6			! pos += step
	sts	macl,r2
	shll	r2
	swap.w	r2,r2
	exts.w	r2,r2			! d * frac >> 15
	add	r2,r1			! s
	muls.w	r1,r10
	mov.l	@r4,r2
	sts	macl,r3
	muls.w	r1,r11
	add	r3,r2
	mov.l	r2,@r4
	mov.l	@(4,r4),r2
	sts	macl,r3
	add	r3,r2
	mov.l	r2,@(4,r4)
	add	r14,r10			! gain ramps
	add	r7,r11
	cmp/hs	r12,r6
	bt	.Lwrap
.Lcont:
	dt	r9
	bf/s	.Lloop
	add	#8,r4
	mov	r6,r0
.Lret:
	mov.l	@r15+,r7
	mov.l	r10,@(8,r7)		! gains written back
	mov.l	r11,@(12,r7)
	mov.l	@r15+,r14
	mov.l	@r15+,r13
	mov.l	@r15+,r12
	mov.l	@r15+,r11
	mov.l	@r15+,r10
	mov.l	@r15+,r9
	rts
	mov.l	@r15+,r8
.Lwrap:
	tst	r13,r13
	bf/s	.Lcont
	sub	r13,r6
	bra	.Lret
	mov	#-1,r0

! void mixjobs(int32_t *acc, int32_t *ein, vjob_t *jobs, uint32_t counts)
!   vjob_t (40 bytes) = { mixv args (32), pcm, pos }; pos is updated (0xFFFFFFFF: sample ended,
!   the job is skipped from then on); each job's n is set to the frame count
!   counts = n_echo_jobs | n_jobs << 8 | frames << 16 | copy_ein << 24
!   acc = sum of all jobs; ein = sum of the first n_echo_jobs (copied when copy_ein)
	.align 2
	.global _mixjobs
_mixjobs:
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov.l	r10,@-r15
	mov.l	r11,@-r15
	mov.l	r12,@-r15
	mov.l	r13,@-r15
	sts.l	pr,@-r15
	mov	r4,r8			! acc
	mov	r5,r9			! ein
	mov	r6,r10			! jobs
	mov	r7,r11			! counts
	mov	r11,r0
	shlr16	r0
	extu.b	r0,r12			! frames
	add	r12,r12			! longs
	mov	r8,r1
	mov	#0,r2
1:
	mov.l	r2,@r1
	dt	r12
	bf/s	1b
	add	#4,r1
	extu.b	r11,r13			! echo jobs
	bsr	.Ljobs
	nop
	! copy acc -> ein
	mov	r11,r0
	shlr16	r0
	shlr8	r0
	tst	r0,r0
	bt	3f
	mov	r11,r0
	shlr16	r0
	extu.b	r0,r12
	add	r12,r12
	mov	r8,r1
	mov	r9,r2
2:
	mov.l	@r1+,r0
	mov.l	r0,@r2
	dt	r12
	bf/s	2b
	add	#4,r2
3:
	mov	r11,r0
	shlr8	r0
	extu.b	r0,r0
	extu.b	r11,r1
	sub	r1,r0
	bsr	.Ljobs
	mov	r0,r13			! remaining jobs
	lds.l	@r15+,pr
	mov.l	@r15+,r13
	mov.l	@r15+,r12
	mov.l	@r15+,r11
	mov.l	@r15+,r10
	mov.l	@r15+,r9
	rts
	mov.l	@r15+,r8

! run r13 jobs from r10 (advancing r10) into acc r8
.Ljobs:
	sts.l	pr,@-r15
	tst	r13,r13
	bt	5f
4:
	mov.l	@(36,r10),r6
	mov	r6,r0
	cmp/eq	#-1,r0
	bt	6f
	mov	r11,r0
	shlr16	r0
	extu.b	r0,r0
	mov.l	r0,@(4,r10)		! n
	mov	r8,r4
	mov.l	@(32,r10),r5
	bsr	_mixv
	mov	r10,r7
	mov.l	r0,@(36,r10)
6:
	dt	r13
	bf/s	4b
	add	#40,r10
5:
	lds.l	@r15+,pr
	rts
	nop

! void expand2(int32_t *buf, uint32_t nh, int32_t *prev)
!   buf holds nh stereo frames mixed at half rate; expand in place to 2*nh frames at full rate:
!   out[2k] = (a[k-1] + a[k]) / 2, out[2k+1] = a[k]; a[-1] = prev[0..1] (updated to a[nh-1])
	.align 2
	.global _expand2
_expand2:
	mov	r5,r1
	add	#-1,r1
	shll2	r1
	shll	r1
	add	r4,r1			! r1 = &a[nh-1]
	mov	r5,r7
	shll2	r7
	shll2	r7
	add	r4,r7			! r7 = &out[2nh] (stores pre-decrement)
	mov.l	@r1,r0
	mov.l	r0,@-r15		! last L
	mov.l	@(4,r1),r0
	mov.l	r0,@-r15		! last R
	add	#-1,r5			! k = nh-1 .. 1
	tst	r5,r5
	bt	2f
1:
	mov.l	@r1,r2			! a[k] L
	mov.l	@(4,r1),r3		! a[k] R
	add	#-8,r1
	mov.l	r3,@-r7
	mov.l	r2,@-r7			! out[2k+1] = a[k]
	mov.l	@(4,r1),r0
	add	r3,r0
	shar	r0
	mov.l	r0,@-r7
	mov.l	@r1,r0
	add	r2,r0
	shar	r0
	dt	r5
	bf/s	1b
	mov.l	r0,@-r7			! out[2k] = (a[k-1] + a[k]) / 2
2:
	mov.l	@r1,r2			! a[0]
	mov.l	@(4,r1),r3
	mov.l	r3,@-r7
	mov.l	r2,@-r7
	mov.l	@(4,r6),r0
	add	r3,r0
	shar	r0
	mov.l	r0,@-r7
	mov.l	@r6,r0
	add	r2,r0
	shar	r0
	mov.l	r0,@-r7
	mov.l	@r15+,r0
	mov.l	r0,@(4,r6)
	mov.l	@r15+,r0
	rts
	mov.l	r0,@r6
