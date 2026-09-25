	.area CODE (ABS)
	.org 0x40C5
START:
	sjp HELPER
	.include "include_part.asm"
AFTER:
	.db 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
	bch START
