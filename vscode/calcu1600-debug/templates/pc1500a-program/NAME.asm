; {{name}}.asm -- a PC-1500A machine-code program (LH5801, sdaslh5801).
;
; F5 assembles it, sets up a PC-1500A (debug.pc1500a), loads it at &7C01
; (the machine-language area every PC-1500A has) and stops at ENTRY.
; After an edit, Build & Load (Cmd+Alt+L / Ctrl+Alt+L) does it again
; without leaving the session. Each CALL &7C01 counts COUNT up by one.

            .area   CODE (ABS)
            .org    0x7C01

ENTRY:      lda     (COUNT)
            inc     a
            sta     (COUNT)
            rtn

COUNT:      .db     0x00
