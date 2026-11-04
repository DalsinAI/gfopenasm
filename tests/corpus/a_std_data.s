; gfasm-args: -Felf
; gfasm-cpu: arm std
	.text
	.global	start,func
start:	ldr	r0,=table
	ldr	r1,=external+8
	ldr	r2,=0x12345678
	ldr	r3,=0x12345678
	ldr	r4,=0xff
	ldr	r5,=-2
	bl	func
	bl	external
	blne	external+16
	b	other
	cmp	r0,#0
	beq	1$
	b	start
1$:	mov	pc,lr
func:	mov	r0,#42
	adr	r1,func
	ldr	r2,func
	str	r2,[pc,#-8]
	mov	pc,lr
	.ltorg
	.align	2
code2:	ldr	r6,=table+4
	movs	r7,#0x101
	.word	1,2
	.long	start,func,external+4,0x12345678,external-$
	.short	1,-1,0xffff,external
	.byte	1,2,"str",0,external
	.align	2,0x55
	.data
table:	.long	start,func,external,code2
	.ascii	"abc"
	.asciz	"esc\t\n\x41"
	.string	"two","strings"
	.align	3
	.quad	0x123456789abcdef0
	.balign	16
	.4byte	1
	.2byte	2
	.space	5
	.space	3,0xaa
	.zero	2
	.balignw 8,0x1234
	.byte	1
	.balignl 16,0xdeadbeef
	.p2align 2
	.long	$
	.balign	64,0x77,4
other:	.long	other
	.bss
buf:	.space	64
	.lcomm	lc,16,8
	.comm	cm,32
	.section .rodata
ro:	.long	ro
	.section .mysect,"adrw"
	.long	1
	.text
	.long	$ - start
	.equ	CONST,0x1000
	.set	OFF,$ + 8 - start
	ldr	r0,[r1,#-OFF]
	mov	r0,#CONST
	add	r0,r0,#CONST+1
