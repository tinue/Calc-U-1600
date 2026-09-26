; ============================================================
; LCDALLON_1500A.ASM -- lights every dot-matrix pixel and every status
; indicator on the PC-1500A's LCD, then waits for BREAK (the ON key,
; pressed while this routine is running) before returning to BASIC.
; Written to visually check LCDDisplayView's indicator/dot-matrix
; sizing (plan.md Phase 2a "LOG panel" / LCD-sizing work) against real
; hardware with every element lit at once.
;
; ENTRY = 0x7C01, outside the module window -- no BASIC-program-space
; reservation needed, same convention as samples/memtest_1500a.asm and
; this project's own adrtest_1500a.asm.
;
; Display-RAM layout (Core/PC1500/PC1500Display.cpp, read directly from
; this emulator's own source, not guessed):
;   0x7600-0x764D  78 bytes, dot-matrix pixel data, columns 0-38 and
;                  78-116 (low/high nibble of each byte respectively)
;   0x764E         SYMB1: bit0=BUSY,1=SHIFT,2=JPN,3=SML,4=romanIII,
;                  5=romanII,6=romanI,7=DEF
;   0x764F         SYMB2: bit0=DE,1=G,2=RAD,4=RESERVE,5=PRO,6=RUN
;   0x7700-0x774D  78 bytes, dot-matrix pixel data, columns 39-77 and
;                  117-155
; Writing 0xFF to every one of those bytes lights all 7 dot-matrix rows
; across all 156 columns and every status icon this Core models (two
; unused bit positions per SYMB byte are harmless).
;
; BREAK poll: IF register at ME1 address 0xF00B, bit 1 -- set on an
; ON-key press while a program is running (PC1500Memory.hpp's own
; setOnKeyPressed() doc comment; this Core does NOT clear it on read,
; confirmed by that same file, so polling it is safe). Polled directly
; here rather than via the ROM's (0xA6) VMJ call-table entry
; (E451, "Test BREAK/ON key"), so this routine has no dependency on
; BASIC-interpreter state and works standalone.
;
; Calling convention (CALL address,X), matching memtest_1500a.asm's own
; convention: X's value on entry is irrelevant. This routine clears the
; BREAK flag it detected (mirroring the ROM's own
; `ani #(0xF00B),0xFD` convention) and returns with carry CLEAR, so
; BASIC does not write X back to the caller's variable -- there is no
; result to report, this is a purely visual test.
;
;   CALL &7C01,X
;
; ============================================================
; Assembly command:
;   sdaslh5801 -plosgff lcdallon_1500a.asm
; ============================================================

ENTRY       .equ    0x7C01

IF_REG      .equ    0xF00B      ; ME1: LH5811 interrupt-flag register
DOTS0       .equ    0x7600      ; dot-matrix half 0
DOTS1       .equ    0x7700      ; dot-matrix half 1
DOTS_LEN    .equ    0x4E        ; 78 bytes per half (also reaches SYMB1/SYMB2
                                ; immediately after DOTS0's 78 bytes)

; ============================================================
            .area   CODE (ABS)
            .org    ENTRY
; ============================================================

START:
; --- Fill dot-matrix half 0 (0x7600-0x764D) plus SYMB1/SYMB2
;     (0x764E-0x764F) with 0xFF -- all four are contiguous, so one
;     80-byte fill covers all of them. ---
            ldi     xh, 0x76
            ldi     xl, 0x00
            ldi     a, 0xFF
FILL0:
            sta     (x)
            inc     x
            cpi     xl, DOTS_LEN + 2    ; 0x50: past SYMB2 too
            bcr     FILL0               ; xl < 0x50 -> keep going

; --- Fill dot-matrix half 1 (0x7700-0x774D) with 0xFF ---
            ldi     xh, 0x77
            ldi     xl, 0x00
FILL1:
            sta     (x)
            inc     x
            cpi     xl, DOTS_LEN
            bcr     FILL1               ; xl < 0x4E -> keep going

; ============================================================
; WAIT_BREAK -- poll IF bit 1 until the ON key is pressed
; ============================================================
WAIT_BREAK:
            lda     #(IF_REG)
            bii     a, 0x02
            bzs     WAIT_BREAK          ; Z=1 (bit clear) -> not pressed yet

; BREAK detected -- clear the flag (mirrors the ROM's own convention)
; and return without writing anything back to the caller's variable.
            ani     #(IF_REG), 0xFD
            rtn
