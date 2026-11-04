; gfasm-args: -Fhunk
; conditional assembly
A	equ	1
B	equ	0
	ifd	A
	dc.b	1
	endc
	ifnd	A
	dc.b	2
	endc
	ifnd	C
	dc.b	3
	endc
	if	A
	dc.b	4
	else
	dc.b	5
	endif
	ifne	B
	dc.b	6
	elseif	A
	dc.b	7
	else
	dc.b	8
	endif
	ifeq	B
	ifgt	A
	dc.b	9
	endc
	iflt	A
	dc.b	10
	endc
	ifle	B
	dc.b	11
	endc
	ifge	B
	dc.b	12
	endc
	endc
	ifc	'abc','abc'
	dc.b	13
	endc
	ifnc	'abc','abd'
	dc.b	14
	endc
	ifeq	1
	ifne	1
	dc.b	15
	endc
	dc.b	16
	endc
	if	A&&B
	dc.b	17
	endc
	if	A||B
	dc.b	18
	endc
	iif	A	dc.b	19
	iif	B	dc.b	20
	ifmacrod	nomac
	dc.b	21
	endc
	ifmacrond	nomac
	dc.b	22
	endc
	if1
	dc.b	23
	endc
	if2
	dc.b	24
	endc
	even
