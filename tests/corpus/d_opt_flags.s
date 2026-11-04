; gfasm-args: -Fhunk -no-opt
start:	move.l	#1,d0
	add.l	#4,d1
	lea	start,a0
	bra	next
next:	clr.l	d2
	cmp.l	#0,d3
	move.w	$12,d4
