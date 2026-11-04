; gfasm-args: -Fhunk
; macros: arguments, \@, \0, NARG, \#, nesting, mexit
push	macro
	move.\0	\1,-(sp)
	endm
pop	macro
	move.\0	(sp)+,\1
	endm
uniq	macro
lab\@	nop
	bra.s	lab\@
	endm
args	macro
	dc.w	NARG,\#
	ifge	NARG-2
	dc.w	\2
	endc
	ifb	\3
	dc.w	$ffff
	else
	dc.w	\3
	endif
	endm
early	macro
	dc.b	1
	ifeq	\1
	mexit
	endc
	dc.b	2
	endm
outer	macro
	inner	\1+1
	endm
inner	macro
	dc.w	\1
	endm
	MACRO	named
	dc.l	\1
	ENDM
start:	push.l	d0
	push.w	a1
	pop.l	d2
	uniq
	uniq
	args
	args	1
	args	1,2
	args	1,2,3
	early	0
	early	1
	even
	outer	5
	named	start
	args	<1,2>,3
	rept	3
	dc.w	REPTN
	endr
	rept	2
	rept	2
	dc.b	REPTN
	endr
	endr
