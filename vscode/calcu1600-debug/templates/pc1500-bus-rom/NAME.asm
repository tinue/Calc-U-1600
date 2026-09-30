; {{name}}.asm -- a PC-1500 ROM (LH5801, sdaslh5801) at &8000 on the 60-pin
; bus, plugged in by debug.pc1500a's bus-rom:.
;
; A real ROM extension usually adds BASIC commands, which the firmware
; finds and activates at power-on; the commands then run the code in the
; ROM. This minimal one only shows the mechanics: F5 assembles it, sets the
; machine up with the ROM, types CALL &8000 (debug.pc1500a's command) and
; stops at a breakpoint you set here. With commands of your own, change
; `command:` to one of them, or use `boot: debug` for the power-on part.
; Build & Load (Cmd+Alt+L / Ctrl+Alt+L) rebuilds it and sets the machine
; up again.
; A ROM that sits only behind PV or PU (like the CE-158's) takes `pv:` /
; `pu:` in bus-rom:.

            .area   CODE (ABS)
            .org    0x8000

ENTRY:      lda     (COUNT)
            inc     a
            sta     (COUNT)
            rtn

COUNT       .equ    0x7C01      ; a byte of RAM: a ROM can't count in itself
