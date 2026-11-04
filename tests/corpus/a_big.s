; gfasm-args: -Felf -big
; gfasm-cpu: arm std
	.text
	.global	f
f:	ldr	r0,=0x12345678
	bl	ext
	mov	pc,lr
	.long	ext,f,0x01020304
	.short	0x0506
	.align	2
	.thumb
	mov	r0,#1
	bl	ext
	bx	lr
	.data
	.long	0x11223344
	.short	0x5566
