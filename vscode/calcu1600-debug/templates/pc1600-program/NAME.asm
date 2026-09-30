; {{name}}.asm -- a PC-1600 machine-code program (Z80, zasm).
;
; F5 assembles it, sets up a plain PC-1600 (debug.pc1600), loads it at
; C0C5H and stops at `start`. After an edit, Build & Load (Cmd+Alt+L /
; Ctrl+Alt+L) does it again without leaving the session.

#target bin

CLS         equ 0112H       ; clear the display
PRTASTR     equ 00EBH       ; print the text at DE up to the byte in A

#code CODE, 0C0C5H

start:
        call CLS
        ld   de, hello
        xor  a
        call PRTASTR
        ret

hello:  defm "Hello from {{name}}"
        defb 0
