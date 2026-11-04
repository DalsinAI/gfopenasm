; gfasm-args: -Felf
; gfasm-cpu: arm std
	.text
	.global	tfunc
	.arm
	adr	r0,tfunc+1
	bx	r0
	.thumb
tfunc:	push	{r4-r7,lr}
	mov	r4,#10
1$:	sub	r4,#1
	bne	1$
	bl	ext
	bl	tlocal
	beq	far
	ldr	r0,lit
	add	r1,pc,#4
	adr	r2,lit
	.align	2
	nop
	mov	r0,r1
	pop	{r4-r7,pc}
	.align	2
lit:	.long	0x11223344
tlocal:	bx	lr
	.space	400
far:	b	far
	.arm
	.align	2
	mov	r0,r0
	ldr	r0,=ext
