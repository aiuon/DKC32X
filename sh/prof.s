! H-interrupt PC sampler (profiling builds): histogram of the interrupted PC in the game code
! (1 KB buckets from _prof_base), then the normal handler.
	.section .sdtext,"ax"
	.align 4
	.global _prof_hint
_prof_hint:
	mov.l	r0,@-r15
	mov.l	r1,@-r15
	mov.l	.Lflag,r1
	mov.l	@r1,r0
	tst	r0,r0
	bt	2f
	mov.l	@(8,r15),r0
	mov.l	.Lbase,r1
	mov.l	@r1,r1
	sub	r1,r0
	mov.l	.Llimit,r1
	cmp/hs	r1,r0
	bt	1f
	shlr8	r0
	shlr	r0
	add	r0,r0
	mov.l	.Lhist,r1
	add	r0,r1
	mov.w	@r1,r0
	add	#1,r0
	bra	2f
	mov.w	r0,@r1
1:
	mov.l	.Lother,r1
	mov.l	@r1,r0
	add	#1,r0
	mov.l	r0,@r1
2:
	mov.l	@r15+,r1
	mov.l	.Lmh,r0
	jmp	@r0
	mov.l	@r15+,r0
	.align 2
.Lbase:	.long	_prof_base
.Llimit:	.long	0x80000
.Lflag:	.long	_prof_on
.Lhist:	.long	_prof_hist
.Lother:	.long	_prof_other
.Lmh:	.long	_master_hint
