; gfasm-args: -Fhunk
	include	"inc/d_inc1.i"
	incdir	"inc"
	include	d_inc2.i
	dc.w	INC1,INC2
	incbin	"inc/blob.bin"
	even
	incbin	"inc/blob.bin",2,3
	even
