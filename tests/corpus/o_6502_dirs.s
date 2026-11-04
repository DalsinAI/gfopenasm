; gfasm-args: -Fbin -cbm-prg
; gfasm-cpu: 6502 oldstyle
	*=$0801
zp = $fb
	dw next,10
	db $9e,"2061",0
next	dw 0
	org $080d
start	lda #<msg
	sta zp
	lda #>msg
	sta zp+1
	ldy #0
:	lda (zp),y
	beq :+
	jsr $ffd2
	iny
	bne :-
:	rts
msg	string "HELLO"
	fcs "AB"
	fill 3,$ea
	dsect
v1	byt
v2	wrd
	dend
	lda v2
	sta v1,x
	if 1
lab	nop
	endif
	jmp lab
	blt start
