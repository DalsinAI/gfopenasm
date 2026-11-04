; gfasm-args: -Fhunk -nosym
	xdef	entry
entry:	lea	datlab(pc),a0
	rts
datlab:	dc.l	entry
other:	dc.l	0
