#target bin
#code CODE, 0xC0C5
start:	call helper
#include "include_part.asm"
after:	db 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
	db "hello world, a long string", 0
	jr start
