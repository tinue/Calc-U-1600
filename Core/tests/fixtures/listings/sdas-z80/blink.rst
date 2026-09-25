                                      1 ; Tiny SC7852 sample for the listing-parser fixture (sdasz80 dialect):
                                      2 ; a relocatable area linked to 0xC0C5, so the .rst holds the final
                                      3 ; addresses while the .lst holds area offsets.
                                      4 	.module blink
                                      5 	.area	CODE (REL)
                                      6 
    0000C0C5                          7 start::
    0000C0C5 21 D8 C0         [10]    8 	ld	hl,#counter
    0000C0C8 06 0A            [ 7]    9 	ld	b,#10
    0000C0CA                         10 loop:
    0000C0CA 34               [11]   11 	inc	(hl)
    0000C0CB CD D1 C0         [17]   12 	call	delay
    0000C0CE 10 FA            [13]   13 	djnz	loop
    0000C0D0 C9               [10]   14 	ret
                                     15 
    0000C0D1                         16 delay:
    0000C0D1 C5               [11]   17 	push	bc
    0000C0D2 06 00            [ 7]   18 	ld	b,#0
    0000C0D4 10 FE            [13]   19 1$:	djnz	1$
    0000C0D6 C1               [10]   20 	pop	bc
    0000C0D7 C9               [10]   21 	ret
                                     22 
    0000C0D8                         23 counter:
    0000C0D8 00                      24 	.db	0
