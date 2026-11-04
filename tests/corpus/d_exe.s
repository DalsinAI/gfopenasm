; gfasm-args: -Fhunkexe
	section	text,code
entry:	lea	datlab,a0
	move.l	#bsslab,d0
	move.l	entry2,d1
	jmp	entry2
entry2:	rts
	section	dat,data_c
datlab:	dc.l	entry,entry2,datlab,bsslab+4
	section	bss1,bss
bsslab:	ds.b	100
