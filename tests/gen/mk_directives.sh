#!/bin/sh
# Writes the hand-made directive/syntax/output tests into tests/corpus.
# (Kept as one script so related variants stay together.)
C=$(cd "$(dirname "$0")/../corpus" && pwd)

cat > "$C/d_labels.s" <<'EOF'
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
EOF

cat > "$C/d_macros.s" <<'EOF'
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
EOF

cat > "$C/d_cond.s" <<'EOF'
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
EOF

cat > "$C/d_data.s" <<'EOF'
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
EOF

cat > "$C/d_sections.s" <<'EOF'
; gfasm-args: -Fhunk
; sections, memory attributes, relocations, symbols
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
entry2:	rts
	section	dat,data
datlab:	dc.l	entry,entry2,datlab,_ext1,bsslab+4
	dc.w	0
	section	bss1,bss
bsslab:	ds.b	100
	section	chipdata,data_c
	dc.w	$1234
	section	fastbss,bss,fast
	ds.l	4
	section	other,code,chip
	nop
	section	attr,data,$12340000
	dc.b	1
	section	text,code
	nop
	data
	dc.b	9
	code
	nop
	bss
	ds.b	1
EOF

cat > "$C/d_sections_elf.s" <<'EOF'
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
EOF

cat > "$C/d_exe.s" <<'EOF'
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
EOF

cat > "$C/d_exe_nosym.s" <<'EOF'
; gfasm-args: -Fhunkexe -nosym
entry:	lea	datlab(pc),a0
	rts
datlab:	dc.l	entry
EOF

cat > "$C/d_obj_nosym.s" <<'EOF'
; gfasm-args: -Fhunk -nosym
	xdef	entry
entry:	lea	datlab(pc),a0
	rts
datlab:	dc.l	entry
other:	dc.l	0
EOF

cat > "$C/d_offsets.s" <<'EOF'
; gfasm-args: -Fhunk
; rs/so/fo, offset sections, structures
	rsreset
s_a	rs.l	1
s_b	rs.w	2
s_c	rs.b	3
s_size	rs.b	0
	rsset	100
t_a	rs.l	1
	clrso
u_a	so.w	1
u_b	so.l	2
u_size	so.b	0
	clrfo
f_a	fo.l	1
f_b	fo.w	1
	offset	0
o_a	ds.l	1
o_b	ds.w	1
o_size	ds.b	0
	section	code,code
	dc.w	s_a,s_b,s_c,s_size,t_a,u_a,u_b,u_size,f_a,f_b,o_a,o_b,o_size,__RS
	move.l	s_b(a0),d0
	lea	o_size(a1),a2
EOF

cat > "$C/d_bin_org.s" <<'EOF'
; gfasm-args: -Fbin
	org	$1000
start:	lea	data(pc),a0
	move.l	#data,d0
	bra.s	start
	jmp	start
	lea	start,a1
data:	dc.l	start,data
	dc.w	*-start
EOF

cat > "$C/d_include.s" <<'EOF'
; gfasm-args: -Fhunk
	include	"inc/d_inc1.i"
	incdir	"inc"
	include	d_inc2.i
	dc.w	INC1,INC2
	incbin	"inc/blob.bin"
	even
	incbin	"inc/blob.bin",2,3
	even
EOF
mkdir -p "$C/inc"
printf 'INC1\tequ\t$1111\n\tdc.w\t1\n' > "$C/inc/d_inc1.i"
printf 'INC2\tequ\t$2222\n\tdc.w\t2\n' > "$C/inc/d_inc2.i"
printf 'ABCDEFGH' > "$C/inc/blob.bin"

cat > "$C/d_cpu.s" <<'EOF'
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
EOF

cat > "$C/d_branches.s" <<'EOF'
; gfasm-args: -Fhunk
; branch relaxation across a large gap
start:	bra	far
	beq	near
	bsr	sub
near:	nop
sub:	rts
	ds.b	200
	bne	start
	bra	start
	ds.b	40000
far:	bra	start
	jmp	start
	jsr	near
	jmp	(start,pc)
	bra	far2
	nop
far2:	rts
EOF

cat > "$C/d_branches020.s" <<'EOF'
; gfasm-args: -Fhunk -m68020
start:	bra	far
	beq	near
	bsr	sub
near:	nop
sub:	rts
	ds.b	200
	bne	start
	ds.b	40000
far:	bra	start
	jmp	start
	fbeq	start
	fbne	near
EOF

cat > "$C/d_opt_flags.s" <<'EOF'
; gfasm-args: -Fhunk -no-opt
start:	move.l	#1,d0
	add.l	#4,d1
	lea	start,a0
	bra	next
next:	clr.l	d2
	cmp.l	#0,d3
	move.w	$12,d4
EOF
