! run65_safe(cpu, key): r8-r13 hold the 65816 registers inside the game code (global register
! variables); save the caller's values around it.
	.text
	.align 2
	.global _run65_safe
_run65_safe:
	mov.l	r8,@-r15
	mov.l	r9,@-r15
	mov.l	r10,@-r15
	mov.l	r11,@-r15
	mov.l	r12,@-r15
	mov.l	r13,@-r15
	mov.l	r14,@-r15
	sts.l	pr,@-r15
	mov.l	.Lrun65,r0
	jsr	@r0
	nop
	lds.l	@r15+,pr
	mov.l	@r15+,r14
	mov.l	@r15+,r13
	mov.l	@r15+,r12
	mov.l	@r15+,r11
	mov.l	@r15+,r10
	mov.l	@r15+,r9
	rts
	mov.l	@r15+,r8
	.align 2
.Lrun65:
	.long	_run65
