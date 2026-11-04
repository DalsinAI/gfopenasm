#!/bin/sh
# Hand-written PowerPC / std-syntax tests for tests/corpus.
C=$(cd "$(dirname "$0")/../corpus" && pwd)

cat > "$C/p_std_data.s" <<'EOF'
# gfasm-args: -Felf
# gfasm-cpu: ppc std
	.text
	.globl	start,func
start:	lis	3,table@ha
	addi	3,3,table@l
	lwz	4,4(3)
	bl	func
	bl	external
	cmpwi	3,0
	beq	1$
	b	start
1$:	blr
func:	li	3,42
	blr
	.data
table:	.long	start,func,external+4,0x12345678
	.short	1,-1,0xffff
	.byte	1,2,"str",0
	.ascii	"abc"
	.asciz	"esc\t\n\x41"
	.string	"two","strings"
	.align	3
	.quad	0x123456789abcdef0
	.float	1.5,-2
	.double	3.14159
	.balign	16
	.4byte	1
	.2byte	2
	.space	5
	.space	3,0xaa
	.zero	2
	.p2align 2
	.long	$
	.bss
buf:	.space	64
	.lcomm	lc,16,3
	.comm	cm,32
	.section .rodata
ro:	.long	ro
	.section .mysect,"adrw"
	.long	1
	.text
	.long	$ - start
EOF

cat > "$C/p_std_macros.s" <<'EOF'
# gfasm-args: -Felf
# gfasm-cpu: ppc std
	.macro	pushr r
	stwu	\r,-4(1)
	.endm
	.macro	add3 d,a,b=1
	addi	\d,\a,\b
	.endm
	.macro	lab
L\@:	b	L\@
	.endm
	.text
	pushr	3
	pushr	31
	add3	3,4,5
	add3	3,4
	lab
	lab
	.rept	3
	nop
	.endr
	.irp	r,3,4,5
	li	\r,0
	.endr
	.set	cnt,1
	.set	cnt,cnt+1
	li	3,cnt
	.equ	x,7
	li	4,x
	.if	x>5
	li	5,1
	.else
	li	5,2
	.endif
	.ifdef	x
	li	6,1
	.endif
	.ifndef	nothere
	li	6,2
	.endif
	.ifeq	x-7
	li	7,1
	.endif
EOF

cat > "$C/p_std_hunk.s" <<'EOF'
# gfasm-args: -Fhunk
# gfasm-cpu: ppc std
	.text
	.globl	start
start:	bl	ext
	b	start
	bl	other
	nop
	.data
data:	.long	start,ext
	.byte	1
	.bss
buf:	.space	16
	.section .text2,"acrx"
other:	blr
EOF

cat > "$C/p_std_bin.s" <<'EOF'
# gfasm-args: -Fbin
# gfasm-cpu: ppc std
	.org	0x10000
start:	lis	3,data@ha
	addi	3,3,data@l
	bl	sub
	b	start
sub:	blr
data:	.long	start,sub,data
	.short	$-start
EOF

cat > "$C/p_mot.s" <<'EOF'
; gfasm-args: -Felf
; gfasm-cpu: ppc mot
	section	code,code
	xdef	start
start	lis	r3,data@ha
	addi	r3,r3,data@l
	bl	sub
	bne	start
sub	blr
	section	data,data
data	dc.l	start,sub
	dc.w	1
	dc.b	"x",0
	even
EOF
