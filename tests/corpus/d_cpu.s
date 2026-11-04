; gfasm-args: -Fhunk
	machine	68020
	extb.l	d0
	mulu.l	d1,d2
	bfextu	d1{2:5},d3
	mc68000
	moveq	#1,d0
	machine	68040
	fmove.x	fp0,fp1
	move16	(a0)+,(a1)+
	mc68060
	plpar	(a0)
	opt	p=68030
	pmove	tc,(a0)
