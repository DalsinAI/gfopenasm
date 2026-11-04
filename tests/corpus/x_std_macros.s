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
