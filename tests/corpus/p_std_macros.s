# gfasm-args: -Felf
# gfasm-cpu: ppc std
	.macro	pushr r
	stwu	\r,-4(1)
	.endm
	.macro	add3 d,a,b=1
	addi	\d,\a,\b
	.endm
	.macro	lab
L\@:	b	L\@
	.endm
	.text
	pushr	3
	pushr	31
	add3	3,4,5
	add3	3,4
	lab
	lab
	.rept	3
	nop
	.endr
	.irp	r,3,4,5
	li	\r,0
	.endr
	.set	cnt,1
	.set	cnt,cnt+1
	li	3,cnt
	.equ	x,7
	li	4,x
	.if	x>5
	li	5,1
	.else
	li	5,2
	.endif
	.ifdef	x
	li	6,1
	.endif
	.ifndef	nothere
	li	6,2
	.endif
	.ifeq	x-7
	li	7,1
	.endif
