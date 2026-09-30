; {{name}}.asm -- a PC-1600 ROM module (Z80, zasm) in page 1, bank 6
; (4000H-7FFFH), plugged into the 60-pin bus by debug.pc1600's bus-rom:.
;
; At power-on the ROM's module scan (SCANMODS, 07C5H) finds the ID bytes
; 43H,16H at 4000H in banks 1-7 and calls each module's +02H entry with a
; function code in A. With `boot: debug` in debug.pc1600, F5 runs that
; boot under the debugger: set a breakpoint in MODRESET and it stops there.
; Build & Load (Cmd+Alt+L / Ctrl+Alt+L) rebuilds the ROM and powers the
; machine on again with it.
;
; The header layout is the one the ROM modules in banks 3 and 5 use; the
; host-drive ROM (firmware/pc1600-hostdrive/hostdrive.asm in Calc-U-1600)
; is a complete example with a file device and BASIC statements.

#target rom
#code ROM, 4000H, 4000H

MODID:  defb 43H,16H            ; module ID
        jp   MODRESET           ; +02H: reset / power functions (A = function)
        ret                     ; +05H: interrupt hook: nothing
        nop
        nop
        scf                     ; +08H: device IOCS 80H..8FH: none
        ret
        nop
        scf                     ; +0BH: spare
        ret
        nop
        jp   NOAUTO             ; +0EH: AUTORUN.BAS search: not supported
        defw DEVTAB             ; +11H: device-name table
        defw TOKTAB             ; +13H: token table
        scf                     ; +15H: file handler: no devices
        ret
        nop

DEVTAB: defb 00H                ; no devices

TOKTAB: defs 39H,00H            ; letter index: no statements
        defb 00H                ; end of the entries

        defb "{{name}}",00H

; Reset / power functions: A = 0..2 -> A := 0, NC; anything else CY.
MODRESET:
        cp   03H
        jr   nc,MODRESETCY
        xor  a
        ret
MODRESETCY:
        scf
        ret

; AUTORUN search: A := 9BH (device not available).
NOAUTO: ld   a,9BH
        ret
