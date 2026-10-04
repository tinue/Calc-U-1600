; ============================================================
; ADRTEST_1500A.ASM -- Real-hardware check of ADR's flag behavior.
; ============================================================
;
; The PC-1500 Technical Reference Manual says ADR (Rreg = Rreg + A,
; 16-bit) changes C/H/Z/V, matching the 8-bit low-byte addition's own
; flags. The emulator's LH5801 core instead PRESERVES the caller's
; flags across ADR: with the manual's reading, the PC-1500's Up/Down-key
; redraw goes wrong. Until this test has run on hardware, that choice is
; provisional; keep it in mind before touching either the core's ADR or
; this file.
;
; This program settles it empirically: SEC (force C=1), then ADR X
; with operands guaranteed to produce NO carry out of the low byte
; (0x00 + 0x00 = 0x00, no carry), then read C back.
;
;   Result 1 (carry survived the ADR)  -> silicon PRESERVES flags ->
;     the manual's ADR row is wrong, and the core is right.
;   Result 0 (carry was cleared)       -> the manual is right, ADR
;     DOES publish the low-byte add's own flags, and the core is
;     wrong; the Up/Down redraw problem would then lie before ADR,
;     most likely in whatever leaves A=0x00 at the ROM's D2AA.
;
; ============================================================
; Calling convention (CALL address,X), the same as
; examples/machine-code/memtest.asm:
;
;   - X's value on entry is irrelevant -- this test needs no input.
;   - This routine always sets carry before RTN, so BASIC always
;     writes the X register back to the variable: X = 1 (carry
;     survived) or X = 0 (carry was cleared).
;   - The same result is also stored at RESULT, PEEK-able
;     independently of the BASIC variable, as a cross-check.
;
;   PRINT PEEK(&7C04)      ; 1 = flags preserved, 0 = flags published
;
; ============================================================
; Assembly command:
;   sdaslh5801 -plosgff adrtest_1500a.asm
; ============================================================

ENTRY       .equ    0x7C01      ; PC-1500A -- same "outside the module
                                 ; window, no BASIC-program-space
                                 ; reservation needed" convention as
                                 ; examples/machine-code/memtest.asm.

; ============================================================
            .area   CODE (ABS)
            .org    ENTRY
; ============================================================

; +0  Two-byte forward branch; lands at ADRTEST, skipping the one
;     result byte. Offset resolved by the assembler.
            bch     ADRTEST

; +2  Result -- read this with PEEK() after CALL, independent of
;     whatever the BASIC variable ends up holding.
RESULT:     .db     0x00

; ============================================================
; ADRTEST -- entered via CALL from BASIC
; ============================================================
ADRTEST:
            sec                 ; force C=1 before the instruction under test
            ldi     a,0x00
            ldi     xh,0x00
            ldi     xl,0x00     ; 0x00 + 0x00 -> the low-byte add itself
                                 ; produces no carry -- if ADR's flags
                                 ; end up matching that add (the
                                 ; manual's claim), C will read 0 here.
            adr     x           ; FD CA -- the instruction under test

            bcr     CARRY_CLEARED   ; C=0 here -> low-byte add's own
                                     ; flags were published (matches
                                     ; the manual)

; Carry survived the ADR -- flags were preserved (matches
; what the core does).
            ldi     a,0x01
            sta     (RESULT)
            ldi     xh,0x00
            ldi     xl,0x01
            sec                 ; carry set -> BASIC writes X back to the variable
            rtn

CARRY_CLEARED:
            ldi     a,0x00
            sta     (RESULT)
            ldi     xh,0x00
            ldi     xl,0x00
            sec                 ; carry set -> BASIC writes X back to the variable
            rtn
