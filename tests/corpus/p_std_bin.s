# gfasm-args: -Fbin
# gfasm-cpu: ppc std
	.org	0x10000
start:	lis	3,data@ha
	addi	3,3,data@l
	bl	sub
	b	start
sub:	blr
data:	.long	start,sub,data
	.short	$-start
