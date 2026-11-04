; gfasm-args: -Fhunk
; branch relaxation across a large gap
start:	bra	far
	beq	near
	bsr	sub
near:	nop
sub:	rts
	ds.b	200
	bne	start
	bra	start
	ds.b	40000
far:	bra	start
	jmp	start
	jsr	near
	bra	far2
	nop
far2:	rts
