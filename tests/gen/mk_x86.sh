#!/bin/sh
# Hand-written 80x86 / std-syntax directive and output tests for tests/corpus.
C=$(cd "$(dirname "$0")/../corpus" && pwd)

cat > "$C/x_std_data.s" <<'EOF'
# gfasm-args: -Felf
# gfasm-cpu: x86 std
	.text
	.globl	start,func
start:	movl	$table,%eax
	movl	table+4,%ebx
	movl	ext(,%eax,4),%ecx
	pushl	$ext
	call	func
	call	ext
	jmp	ext
	je	ext
	jne	1f
	jmp	start
1:	jmp	*tab(,%eax,4)
	movl	$CONST,%edx
	addl	$CONST,%esi
	cmpl	$fwd,%edi
	leal	-8(%ebp),%eax
	movb	$'x',(%eax)
	loop	start
	jecxz	func
func:	ret
	.align	16
	.long	ext,start,ext+4,ext-.
	.word	ext,1,-1
	.byte	1,2,"str",0
	.balign	4,0x90
	.p2align 3
	.short	2
	.data
table:	.long	1,2,3
tab:	.long	start,func
	.ascii	"abc"
	.asciz	"esc\t\n\x41"
	.align	3
	.quad	0x123456789abcdef0
	.float	1.5
	.double	2.5
	.balign	16,0xcc
	.space	5
	.space	3,0xaa
	.long	.
	.bss
buf:	.space	64
	.lcomm	lc,16,8
	.comm	cm,32
	.section .rodata
ro:	.long	ro
	.text
	.equ	CONST,0x1234
	.set	fwd,100
	.long	. - start
EOF

cat > "$C/x_std_macros.s" <<'EOF'
# gfasm-args: -Felf
# gfasm-cpu: x86 std
	.macro	save r
	pushl	%\r
	.endm
	.macro	addto d,v=1
	addl	$\v,%\d
	.endm
	.text
	save	eax
	addto	ebx,5
	addto	ecx
	.rept	3
	nop
	.endr
	.irp	reg,eax,ebx,ecx
	xorl	%\reg,%\reg
	.endr
	.if	1
	incl	%eax
	.else
	decl	%eax
	.endif
lab:	jmp	lab
2:	jmp	2b
	jmp	3f
	.space	200
3:	ret
EOF

cat > "$C/x_bin.s" <<'EOF'
# gfasm-args: -Fbin
# gfasm-cpu: x86 std
	.org	0x7c00
start:	cli
	xorw	%ax,%ax
	movw	%ax,%ds
	movl	$msg,%esi
	call	print
	jmp	.
print:	lodsb
	testb	%al,%al
	jz	done
	movb	$0x0e,%ah
	int	$0x10
	jmp	print
done:	ret
msg:	.asciz	"hello"
	.space	0x7c00+510-.
	.word	0xaa55
EOF

cat > "$C/x_code16.s" <<'EOS'
# gfasm-args: -Fbin
# gfasm-cpu: x86 std
	.code16
start:	movw	%ax,%bx
	movl	%eax,%ebx
	movw	(%bx),%ax
	movw	4(%bp,%si),%cx
	movl	(%eax),%ebx
	jmp	.+0x100
	call	start
	pushw	$1
	pushl	$1000
	je	.+0x200
	jne	start
	movw	$0x1234,%ax
	movw	0x10,%ax
	push	%ds
	ret
	.code32
	movl	%eax,%ebx
	jmp	start
EOS
