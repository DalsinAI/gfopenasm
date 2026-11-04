; gfasm-args: -Fhunkexe -nosym
entry:	lea	datlab(pc),a0
	rts
datlab:	dc.l	entry
