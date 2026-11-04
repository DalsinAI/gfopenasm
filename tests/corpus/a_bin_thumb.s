; gfasm-args: -Fbin -thumb -m7tdmi
; gfasm-cpu: arm std
start:	mov	r0,#0
	ldr	r1,data
	add	r0,r0,r1
	b	start
	bl	start
	.align	2
data:	.long	start
	.arm
	.align	2
	mov	r0,#1
	ldr	r1,=0x11112222
