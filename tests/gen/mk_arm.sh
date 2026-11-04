#!/bin/sh
# Hand-written ARM / std-syntax directive and output tests for tests/corpus.
C=$(cd "$(dirname "$0")/../corpus" && pwd)

cat > "$C/a_std_data.s" <<'EOF'
; gfasm-args: -Felf
; gfasm-cpu: arm std
	.text
	.global	start,func
start:	ldr	r0,=table
	ldr	r1,=external+8
	ldr	r2,=0x12345678
	ldr	r3,=0x12345678
	ldr	r4,=0xff
	ldr	r5,=-2
	bl	func
	bl	external
	blne	external+16
	b	other
	cmp	r0,#0
	beq	1$
	b	start
1$:	mov	pc,lr
func:	mov	r0,#42
	adr	r1,func
	ldr	r2,func
	str	r2,[pc,#-8]
	mov	pc,lr
	.ltorg
	.align	2
code2:	ldr	r6,=table+4
	movs	r7,#0x101
	.word	1,2
	.long	start,func,external+4,0x12345678,external-$
	.short	1,-1,0xffff,external
	.byte	1,2,"str",0,external
	.align	2,0x55
	.data
table:	.long	start,func,external,code2
	.ascii	"abc"
	.asciz	"esc\t\n\x41"
	.string	"two","strings"
	.align	3
	.quad	0x123456789abcdef0
	.balign	16
	.4byte	1
	.2byte	2
	.space	5
	.space	3,0xaa
	.zero	2
	.balignw 8,0x1234
	.byte	1
	.balignl 16,0xdeadbeef
	.p2align 2
	.long	$
	.balign	64,0x77,4
other:	.long	other
	.bss
buf:	.space	64
	.lcomm	lc,16,8
	.comm	cm,32
	.section .rodata
ro:	.long	ro
	.section .mysect,"adrw"
	.long	1
	.text
	.long	$ - start
	.equ	CONST,0x1000
	.set	OFF,$ + 8 - start
	ldr	r0,[r1,#-OFF]
	mov	r0,#CONST
	add	r0,r0,#CONST+1
EOF

cat > "$C/a_std_macros.s" <<'EOF'
; gfasm-args: -Felf
; gfasm-cpu: arm std
	.macro	pushr r
	stmfd	sp!,{\r}
	.endm
	.macro	add3 d,a,b=1
	add	\d,\a,#\b
	.endm
	.text
	pushr	r4
	add3	r0,r1,5
	add3	r2,r3
	.rept	3
	nop
	.endr
	.irp	reg,r1,r2,r3
	mov	\reg,#0
	.endr
	.irpc	n,123
	mov	r\n,#\n
	.endr
	.if	1
	mov	r0,#1
	.else
	mov	r0,#2
	.endif
	.ifdef	undefined_sym
	mov	r0,#3
	.endif
	.ifndef	undefined_sym
	mov	r0,#4
	.endif
	.ifeq	2-2
	mov	r0,#5
	.endif
lab:	b	lab
2$:	b	2$
	.local	lab
EOF

cat > "$C/a_bin.s" <<'EOF'
; gfasm-args: -Fbin -opt-adr -m250
; gfasm-cpu: arm std
	.org	0x8000
start:	adr	r0,start
	adr	r1,far
	adrl	r2,start
	mov	r3,#far-start
	ldr	r4,=start
	ldr	r6,=0xff00
	add	r7,r7,#0x10001
	sub	r8,r8,#0x101
	mvn	r9,#0x101
	b	far
	bl	start
	teqp	pc,#0
	swi	0x20000
	.align	2
	.space	0x1200
far:	ldr	r0,far2
	adr	r1,start
	mov	pc,lr
	.byte	1
	.align	2,0xff
far2:	.long	far
EOF

cat > "$C/a_big.s" <<'EOF'
; gfasm-args: -Felf -big
; gfasm-cpu: arm std
	.text
	.global	f
f:	ldr	r0,=0x12345678
	bl	ext
	mov	pc,lr
	.long	ext,f,0x01020304
	.short	0x0506
	.align	2
	.thumb
	mov	r0,#1
	bl	ext
	bx	lr
	.data
	.long	0x11223344
	.short	0x5566
EOF

cat > "$C/a_thumb.s" <<'EOF'
; gfasm-args: -Felf
; gfasm-cpu: arm std
	.text
	.global	tfunc
	.arm
	adr	r0,tfunc+1
	bx	r0
	.thumb
tfunc:	push	{r4-r7,lr}
	mov	r4,#10
1$:	sub	r4,#1
	bne	1$
	bl	ext
	bl	tlocal
	beq	far
	ldr	r0,lit
	add	r1,pc,#4
	adr	r2,lit
	.align	2
	nop
	mov	r0,r1
	pop	{r4-r7,pc}
	.align	2
lit:	.long	0x11223344
tlocal:	bx	lr
	.space	400
far:	b	far
	.arm
	.align	2
	mov	r0,r0
	ldr	r0,=ext
EOF

cat > "$C/a_bin_thumb.s" <<'EOF'
; gfasm-args: -Fbin -thumb -m7tdmi
; gfasm-cpu: arm std
start:	mov	r0,#0
	ldr	r1,data
	add	r0,r0,r1
	b	start
	bl	start
	.align	2
data:	.long	start
	.arm
	.align	2
	mov	r0,#1
	ldr	r1,=0x11112222
EOF
