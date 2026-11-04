; gfasm-args: -Felf
; gfasm-cpu: ppc mot
	section	code,code
	xdef	start
start	lis	r3,data@ha
	addi	r3,r3,data@l
	bl	sub
	bne	start
sub	blr
	section	data,data
data	dc.l	start,sub
	dc.w	1
	dc.b	"x",0
	even
