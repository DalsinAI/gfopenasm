; gfasm-args: -Fhunk -m68020 -m68881
start:	bra	far
	beq	near
	bsr	sub
near:	nop
sub:	rts
	ds.b	200
	bne	start
	ds.b	40000
far:	bra	start
	jmp	start
	fbeq	start
	fbne	near
