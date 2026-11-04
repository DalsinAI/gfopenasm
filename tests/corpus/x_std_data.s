# gfasm-args: -Felf
# gfasm-cpu: x86 std
	.text
	.globl	start,func
start:	movl	$table,%eax
	movl	table+4,%ebx
	movl	ext(,%eax,4),%ecx
	pushl	$ext
	call	func
	call	ext
	jmp	ext
	je	ext
	jne	1f
	jmp	start
1:	jmp	*tab(,%eax,4)
	movl	$CONST,%edx
	addl	$CONST,%esi
	cmpl	$fwd,%edi
	leal	-8(%ebp),%eax
	movb	$'x',(%eax)
	loop	start
	jecxz	func
func:	ret
	.align	16
	.long	ext,start,ext+4,ext-.
	.word	ext,1,-1
	.byte	1,2,"str",0
	.balign	4,0x90
	.p2align 3
	.short	2
	.data
table:	.long	1,2,3
tab:	.long	start,func
	.ascii	"abc"
	.asciz	"esc\t\n\x41"
	.align	3
	.quad	0x123456789abcdef0
	.float	1.5
	.double	2.5
	.balign	16,0xcc
	.space	5
	.space	3,0xaa
	.long	.
	.bss
buf:	.space	64
	.lcomm	lc,16,8
	.comm	cm,32
	.section .rodata
ro:	.long	ro
	.text
	.equ	CONST,0x1234
	.set	fwd,100
	.long	. - start
