; Tiny SC7852 sample for the listing-parser fixture (sdasz80 dialect):
; a relocatable area linked to 0xC0C5, so the .rst holds the final
; addresses while the .lst holds area offsets.
	.module blink
	.area	CODE (REL)

start::
	ld	hl,#counter
	ld	b,#10
loop:
	inc	(hl)
	call	delay
	djnz	loop
	ret

delay:
	push	bc
	ld	b,#0
1$:	djnz	1$
	pop	bc
	ret

counter:
	.db	0
