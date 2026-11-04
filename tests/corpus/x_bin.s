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
