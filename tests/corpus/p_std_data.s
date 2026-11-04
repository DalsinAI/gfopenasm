# gfasm-args: -Felf
# gfasm-cpu: ppc std
	.text
	.globl	start,func
start:	lis	3,table@ha
	addi	3,3,table@l
	lwz	4,4(3)
	bl	func
	bl	external
	cmpwi	3,0
	beq	1$
	b	start
1$:	blr
func:	li	3,42
	blr
	.data
table:	.long	start,func,external+4,0x12345678
	.short	1,-1,0xffff
	.byte	1,2,"str",0
	.ascii	"abc"
	.asciz	"esc\t\n\x41"
	.string	"two","strings"
	.align	3
	.quad	0x123456789abcdef0
	.float	1.5,-2
	.double	3.14159
	.balign	16
	.4byte	1
	.2byte	2
	.space	5
	.space	3,0xaa
	.zero	2
	.p2align 2
	.long	$
	.bss
buf:	.space	64
	.lcomm	lc,16,3
	.comm	cm,32
	.section .rodata
ro:	.long	ro
	.section .mysect,"adrw"
	.long	1
	.text
	.long	$ - start
