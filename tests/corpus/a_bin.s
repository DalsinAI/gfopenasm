; gfasm-args: -Fbin -opt-adr -m250
; gfasm-cpu: arm std
	.org	0x8000
start:	adr	r0,start
	adr	r1,far
	adrl	r2,start
	mov	r3,#far-start
	ldr	r4,=start
	ldr	r6,=0xff00
	add	r7,r7,#0x10001
	sub	r8,r8,#0x101
	mvn	r9,#0x101
	b	far
	bl	start
	teqp	pc,#0
	swi	0x20000
	.align	2
	.space	0x1200
far:	ldr	r0,far2
	adr	r1,start
	mov	pc,lr
	.byte	1
	.align	2,0xff
far2:	.long	far
