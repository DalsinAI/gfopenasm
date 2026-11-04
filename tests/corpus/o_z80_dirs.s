; gfasm-args: -Fbin
; gfasm-cpu: z80 oldstyle
	org $8000
start	ld a,1
.loc	jr .loc
loop$	djnz loop$
	byte 1,2
	db "AB",'C',0
	dw $1234,start
	defl $12345678
	defp $123456
	text "hi"
	string "z"
	str "r"
	fcs "ab"
	blk 3
	blk 2,$ff
	blkw 2,$1234
	ds 2
	align 2
	even
val	equ 5
val2 = val*2
	db val2,start>>8,start&255
	dw start.loc
	abyte 1,"AB",2
	byt
	wrd
	data 7
	fill 2
	macro mym,a,b
	ld a,\1
	ld b,\b
	endm
	mym 1,2
mm	macro
	nop
	endm
	mm
	rept 2
	inc a
	endr
	if val>3
	db 1
	else
	db 2
	endif
	ifdef val
	db 3
	endif
	ld a,$&255:db $&255
g2	nop
.l1	jr .l1
	dw g2.l1,start.loc
	inline
.l1	nop
	jr .l1
	einline
x$	djnz x$
	ld a,(ix+val)
	out (#a0),a
	ld a,#10
	db 010,08,%1010,1010b,0ah,17q,2#11
	db <start,>start
	ld a,"\\"
	db "a\\n"
	db
	string "x",'y'
  indented:
	nop
	org $8100
	db 1
