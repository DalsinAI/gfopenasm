# gfasm-args: -Fhunk
# gfasm-cpu: ppc std
	.text
	.globl	start
start:	bl	ext
	b	start
	bl	other
	nop
	.data
data:	.long	start,ext
	.byte	1
	.bss
buf:	.space	16
	.section .text2,"acrx"
other:	blr
