; gfasm-args: -Felf
; gfasm-cpu: arm std
	.macro	pushr r
	stmfd	sp!,{\r}
	.endm
	.macro	add3 d,a,b=1
	add	\d,\a,#\b
	.endm
	.text
	pushr	r4
	add3	r0,r1,5
	add3	r2,r3
	.rept	3
	nop
	.endr
	.irp	reg,r1,r2,r3
	mov	\reg,#0
	.endr
	.irpc	n,123
	mov	r\n,#\n
	.endr
	.if	1
	mov	r0,#1
	.else
	mov	r0,#2
	.endif
	.ifdef	undefined_sym
	mov	r0,#3
	.endif
	.ifndef	undefined_sym
	mov	r0,#4
	.endif
	.ifeq	2-2
	mov	r0,#5
	.endif
lab:	b	lab
2$:	b	2$
	.local	lab
