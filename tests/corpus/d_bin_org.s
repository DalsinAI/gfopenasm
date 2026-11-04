; gfasm-args: -Fbin
	org	$1000
start:	lea	data(pc),a0
	move.l	#data,d0
	bra.s	start
	jmp	start
	lea	start,a1
data:	dc.l	start,data
	dc.w	*-start
