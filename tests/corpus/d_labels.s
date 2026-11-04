; gfasm-args: -Fhunk
; labels, locals, colons, equates, set, =
	xdef	start,glob2
start:	moveq	#0,d0
.loop	addq.l	#1,d0
	cmp.l	#10,d0
	bne.s	.loop
loop2$	dbf	d1,loop2$
glob2::	nop
.loop	bra.s	.loop
second
	rts
	nop
third:	rts
VAL	equ	42
VAL2	=	VAL*2
VAL3=VAL2+1
cnt	set	1
cnt	set	cnt+1
	dc.w	VAL,VAL2,VAL3,cnt
	dc.l	start,glob2,second,third
	dc.w	third-start
	dc.b	"string",0,'c'
	even
fwd	equ	later-start
	dc.l	fwd
later	dc.w	*-start
