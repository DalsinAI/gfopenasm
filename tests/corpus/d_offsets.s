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
