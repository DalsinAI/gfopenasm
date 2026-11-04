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
