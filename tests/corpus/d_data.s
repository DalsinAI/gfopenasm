; gfasm-args: -Fhunk
; data directives
	dc.b	1,2,-1,255,"abc",'x'
	dc.w	1,-1,$ffff,"ab"
	dc.l	1,-1,$12345678,"abcd",'x'
	dc	5
	ds.b	3
	ds.w	2
	ds.l	1
	dcb.b	3,$aa
	dcb.w	2,$1234
	dcb.l	2,-2
	blk.b	2,7
	blk.w	1
	dc.s	1.5,-2,0.1
	dc.d	3.14159265358979,1e100
	dc.x	-1.5,1.0e-10
	dcb.s	2,0.5
	db	1,2
	dw	3
	dl	4
	dr.w	*,*+2,*+10
	dr.l	*+4
	dc.b	"it's",0
	dc.b	'say "hi"',0
	dc.b	"x""y",0
	even
	dc.b	1
	cnop	0,4
	dc.b	2
	cnop	2,8
	dc.b	3
	align	3
	dc.b	4
	odd
	dc.b	5
	odd
	even
	dc.w	1+2*3,(1+2)*3,1<<4,$100>>4,7&3,5|8,5^3,-5/2,17//5,~0
	dc.l	1=1,1<2,2>1,1<=1,2>=3,1!=1,1&&0,1||0,!0,!3
