; gfasm-args: -Felf
	xref	_ext1,_ext2
	xdef	entry,datlab,bsslab
	section	text,code
entry:	lea	datlab,a0
	move.l	#bsslab,d0
	jsr	_ext1
	bsr	_ext2
	lea	_ext1(pc),a1
	move.l	entry2,d1
	jmp	entry2
local1:	nop
entry2:	rts
	section	dat,data
datlab:	dc.l	entry,entry2,datlab,_ext1,bsslab+4
	dc.w	0
	section	bss1,bss
bsslab:	ds.b	100
