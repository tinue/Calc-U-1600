; ============================================================
; MEMTEST_BANK.ASM -- Bank-presence memory validator for the PC-1500A
;                     with a CE-163-style banked low-16K memory module
; ============================================================
;
; PC-1500A ONLY. Builds on the plain memtest.asm sweep (write
; 0x55/0xAA/0xFF/0x00 to every byte of a range, read each back, compare)
; and adds, for a machine carrying a bank-switched low-16K memory module:
;
;   - A split run, so the fixed (non-switched) RAM is tested only ONCE:
;       * Phase 1 -- conventional RAM, swept once:
;           no banks : RAM_START_H .. RAM_END_H   (whole user RAM)
;           banks    : 0x4000      .. RAM_END_H   (skip the switched window)
;       * Phase 2 -- for every requested bank, the switched window
;         0x0000..0x3FFF only.
;   - Bank-presence validation. After the per-bank pattern sweeps, every
;     bank is filled with its own marker byte (0x90 + bank), then every
;     bank is read back. A fake / address-aliased bank (asking for more
;     banks than the module physically has) shows up here: its marker
;     write lands on some real bank and overwrites that bank's marker, so
;     the read-back of the real bank fails -> "Bank mismatch: n".
;   - Progress display "Bank: N / Pass: M|Mark|Check" on the LCD via the
;     ROM character routines.
;   - Breakable via the ON key (ROM vector VMJ 0xA6).
;
; Bank switching is a bare strobe: writing ANY value to 0x6800 + N
; selects bank N (the low address bits carry the bank number; the value
; written is irrelevant). The CE-163 has 2 banks; modern recreations
; extend the same strobe to as many as 16.
; This test is destructive: it clobbers everything in the user RAM area,
; RESERVE key assignments, the BASIC program and any DIMmed / long-name
; variables.
; The 26 fixed scalars A-Z / A$-Z$ live in the fixed variable table and
; survive, so the X$ result below is safe to read back.
;
; At normal completion, on a fault, and on BREAK, if any bank switching
; was done (BANK_COUNT > 0) the routine selects bank 0 again before
; returning, so the machine is never left on an arbitrary bank.
;
; This program lives in the PC-1500A's fixed machine-language area
; (0x7C01-0x7FFF), outside the switched window, so it keeps running
; while banks change underneath it. There is no equivalent resident
; area on a plain PC-1500, so this build is PC-1500A-only.
;
; --- Calling convention --
;   CALL &7C01,X$ (arguments are passed via a string variable)
;
;   X$ = "<passes>[.<banks>]", e.g.:
;     X$="3"   / X$="3.0"  -> 3 passes of conventional RAM, no banks
;     X$="3.1"             -> + 3 passes of bank 0, then mark/check bank 0
;     X$="3.8"             -> + 3 passes of each of banks 0..7, then
;                              mark/check all 8
;   <banks> is a single digit 0..9.
;
;   On return, X$ holds a short human-readable result (also shown as the
;   final LCD frame):
;     "Test ok"              -- all sweeps clean
;     "Not run"              -- passes parsed as 0
;     "Fail B<n> &<HHHH>"    -- cell mismatch in bank n at address HHHH
;     "Bank mismatch: <n>"   -- bank n did not hold its own marker on
;                              read-back (fake / aliased bank)
;     "Break"                -- interrupted via the ON key
;
;   PEEKable detail (fixed addresses -- this build always loads at 0x7C01,
;   the two bytes there being the bch over this result area):
;     &7C03  ERR_FLAG    0=clean 1=cell fault 2=not run 3=break
;                        4=bank mismatch
;     &7C04  ERR_ADDR_H  bad address, high byte
;     &7C05  ERR_ADDR_L  bad address, low byte
;     &7C06  ERR_EXPCT   expected byte
;     &7C07  ERR_ACTUAL  byte read back
;     &7C08  BANK_AT_ERR bank the fault / mismatch was found in
;
; Assembly command:
;   sdaslh5801 -plosgff memtest_bank.asm
;
; ============================================================

ENTRY           .equ    0x7C01      ; PC-1500A machine-language area

RAM_START_H     .equ    0x7863      ; page of first valid RAM byte (low byte 0):
                                     ; 0x40 on a stock PC-1500A, 0x00 with a
                                     ; 16K module in the low window
RAM_END_H       .equ    0x7864      ; page of first invalid RAM byte (low byte 0)

BANK_SEL        .equ    0x6800      ; bank-select strobe base: a write to
                                     ; BANK_SEL + N selects bank N (the value
                                     ; written is irrelevant)
CURSOR_PTR      .equ    0x7875      ; ROM's LCD cursor-column pointer (0-0x9C)

; ============================================================
            .area   CODE (ABS)
            .org    ENTRY
; ============================================================

; +0  forward branch over the PEEKable result bytes to MEMTEST.
            bch     MEMTEST

; +2  Result area -- read these with PEEK() after CALL.
ERR_FLAG:       .db     0x00
ERR_ADDR_H:     .db     0x00
ERR_ADDR_L:     .db     0x00
ERR_EXPCT:      .db     0x00
ERR_ACTUAL:     .db     0x00
BANK_AT_ERR:    .db     0x00

; ============================================================
; MEMTEST -- entry from BASIC as CALL &7C01,X$
; ============================================================
MEMTEST:
; On entry: X = address of X$'s data, A = allocated size (16). Keep that
; size as the hard upper bound on how many buffer bytes we may scan --
; the string may only be terminated by running into that padding.
            sta     (ALLOC_SIZE)
            lda     (ALLOC_SIZE)
            sta     (SCAN_LEFT)

            ldi     a,0x00
            sta     (PASS_COUNT)
            sta     (BANK_COUNT)

; --- Parse the pass-count digits -----------------------------------
PARSE_INT_LOOP:
            lda     (SCAN_LEFT)
            cpi     a,0x00
            bzs     PARSE_INT_DONE
            lda     (x)
            cpi     a,0x30              ; '0'
            bcr     PARSE_INT_DONE      ; char < '0' -> not a digit
            cpi     a,0x3A              ; '9'+1
            bcs     PARSE_INT_DONE      ; char > '9' -> not a digit
            sec
            sbi     a,0x30
            sta     (TEMP_DIGIT)
            sjp     MUL10ADD
            inc     x
            lda     (SCAN_LEFT)
            dec     a
            sta     (SCAN_LEFT)
            bch     PARSE_INT_LOOP
PARSE_INT_DONE:

; --- Optional ".<bank-count>" (single digit) ----------------------
            lda     (SCAN_LEFT)
            cpi     a,0x00
            bzs     PARSE_DONE
            lda     (x)
            cpi     a,0x2E              ; '.'
            bzr     PARSE_DONE          ; not '.' -> no bank part
            inc     x
            lda     (SCAN_LEFT)
            dec     a
            sta     (SCAN_LEFT)
            cpi     a,0x00
            bzs     PARSE_DONE
            lda     (x)
            cpi     a,0x30
            bcr     PARSE_DONE
            cpi     a,0x3A
            bcs     PARSE_DONE
            sec
            sbi     a,0x30
            sta     (BANK_COUNT)
PARSE_DONE:

; --- Zero passes -> "not run" ------------------------------------
            lda     (PASS_COUNT)
            bzr     HAVE_COUNT
            ldi     a,0x02
            sta     (ERR_FLAG)
            jmp     RETURN_NOTRUN

HAVE_COUNT:
; ============================================================
; Phase 1 -- conventional (non-switched) RAM, swept ONCE
; ============================================================
            ldi     a,0x00
            sta     (BANK_IDX)
            sta     (DISP_SHOWBANK)     ; 0 -> "Pass:" only, no "Bank:"
            sta     (DISP_KIND)         ; 0 -> numeric pass

            lda     (BANK_COUNT)
            bzs     P1_WHOLE
            ldi     a,0x40             ; module present: skip switched window
            bch     P1_START_SET
P1_WHOLE:
            lda     (RAM_START_H)      ; no banks: whole user RAM
P1_START_SET:
            sta     (SWP_START_H)
            lda     (RAM_END_H)
            sta     (SWP_END_H)
            ldi     a,0x01
            sta     (PASS_IDX)
            sjp     RUN_PASSES

            lda     (BANK_COUNT)
            bzr     PHASE2
            jmp     FINISH_OK

; ============================================================
; Phase 2 -- one outer PHASE loop (0=pattern 1=mark 2=check), inner
; BANK loop 0..BANK_COUNT-1. Pattern sweeps run first for every bank;
; then every bank is marked; then every bank is checked. Mark and
; check are separate full traversals on purpose -- an aliased bank's
; marker write lands on a real bank, and only the later check reveals
; it.
; ============================================================
PHASE2:
            ldi     a,0x01
            sta     (DISP_SHOWBANK)
            ldi     a,0x00
            sta     (PHASE)
            ldi     a,0x00
            sta     (SWP_START_H)       ; every bank uses the switched window
            ldi     a,0x40             ; 0x0000..0x4000
            sta     (SWP_END_H)
PHASE_LOOP:
            lda     (PHASE)
            sta     (DISP_KIND)        ; 0/1/2 -> numeric / "Mark" / "Check"
            ldi     a,0x00
            sta     (BANK_IDX)
BANK_LOOP:
            lda     (BANK_IDX)
            cpa     (BANK_COUNT)
            bcs     PHASE_NEXT         ; BANK_IDX >= BANK_COUNT -> phase done
            sjp     CHECK_BREAK
            bzs     BANK_GO
            jmp     BREAK_HIT
BANK_GO:
            sjp     SEL_BANK

            lda     (PHASE)
            bzs     BANK_PATTERN
; --- mark / check: marker byte -> uh, then FILL or VERIFY ---
            sjp     DISPLAY_STATUS
            lda     (BANK_IDX)
            rec
            adi     a,0x90            ; marker = 0x90 + bank
            sta     uh
            lda     (PHASE)
            cpi     a,0x01
            bzs     BANK_FILL
            sjp     VERIFY            ; mismatch -> jmp BUILD_RESULT (no return)
            bch     BANK_ADV
BANK_FILL:
            sjp     FILL
            bch     BANK_ADV
BANK_PATTERN:
            ldi     a,0x01
            sta     (PASS_IDX)
            sjp     RUN_PASSES         ; displays internally
BANK_ADV:
            lda     (BANK_IDX)
            inc     a
            sta     (BANK_IDX)
            bch     BANK_LOOP
PHASE_NEXT:
            lda     (PHASE)
            inc     a
            sta     (PHASE)
            cpi     a,0x03
            bcr     PHASE_LOOP

FINISH_OK:
            sjp     SEL_BANK0
            ldi     a,0x00
            sta     (ERR_FLAG)
            jmp     RETURN_OK

; ============================================================
; RUN_PASSES -- PASS_COUNT sweeps of the 4 patterns over
; [SWP_START_H:00, SWP_END_H:00). Displays each pass. On a cell
; mismatch jumps to ERROR (never returns here). Both range ends are
; page-aligned, so the tail only has to compare the high byte.
; ============================================================
RUN_PASSES:
            lda     (PASS_COUNT)
            sta     ul
            dec     ul
            lda     (SWP_END_H)
            sta     yh
            ldi     yl,0x00

RUN_OUTER:
            sjp     CHECK_BREAK
            bzs     RO_GO
            jmp     BREAK_HIT_INPASS
RO_GO:
            sjp     DISPLAY_STATUS
            ldi     a,0x00
            sta     (PAT_IDX)
RUN_PAT:
            ldi     xh,>PAT_TBL
            ldi     xl,<PAT_TBL
            lda     (PAT_IDX)
            adr     x
            lda     (x)
            sta     uh                 ; uh = current pattern
            lda     (SWP_START_H)
            sta     xh
            ldi     xl,0x00
RUN_LOOP:
            lda     uh
            sta     (x)
            lda     (x)
            cpa     uh
            bzr     ERROR
            inc     x
            lda     xh
            cpa     yh
            bcr     RUN_LOOP           ; xh < end page -> keep sweeping
            sjp     CHECK_BREAK
            bzs     RP_GO
            jmp     BREAK_HIT_INPASS
RP_GO:
            lda     (PAT_IDX)
            inc     a
            sta     (PAT_IDX)
            cpi     a,0x04
            bcr     RUN_PAT            ; PAT_IDX < 4 -> next pattern

            lda     (PASS_IDX)
            inc     a
            sta     (PASS_IDX)
            lop     ul,RUN_OUTER
            rtn

; ============================================================
; ERROR -- first cell mismatch. A = value read, X = bad address,
; uh = pattern written. Never returns to RUN_PASSES.
; ============================================================
ERROR:
            sta     (ERR_ACTUAL)
            lda     uh
            sta     (ERR_EXPCT)
            lda     xh
            sta     (ERR_ADDR_H)
            lda     xl
            sta     (ERR_ADDR_L)
            lda     (BANK_IDX)
            sta     (BANK_AT_ERR)
            ldi     a,0x01
            sta     (ERR_FLAG)
            sjp     SEL_BANK0          ; ERR_* saved -- safe to clobber regs
            pop     x                  ; drop RUN_PASSES's stranded return addr
            jmp     BUILD_RESULT

; ============================================================
; CHECK_BREAK -- poll the ON key. Returns with Z=0 if pressed, Z=1 if
; not; the caller branches to a BREAK_HIT entry itself. (Deliberately
; returns rather than jumping straight out, so its own return address
; is not stranded on the stack.)
; ============================================================
CHECK_BREAK:
            vmj     0xA6
            rtn

; Two entry points, differing only in stack state on arrival:
;   BREAK_HIT_INPASS -- reached by a plain jmp from *inside* RUN_PASSES,
;     which was entered by SJP, so its return address is stranded and
;     must be dropped first.
;   BREAK_HIT -- reached by a plain jmp from the Phase-2 bank loop
;     (inline in MEMTEST), where only BASIC's own return address is on
;     the stack -- nothing to drop.
BREAK_HIT_INPASS:
            pop     x                  ; drop RUN_PASSES's stranded return addr
BREAK_HIT:
            sjp     SEL_BANK0
            ldi     a,0x03
            sta     (ERR_FLAG)
            jmp     RETURN_BREAK

; ============================================================
; SEL_BANK -- strobe BANK_SEL + (BANK_IDX). Value written = 0.
; BANK_IDX is 0..9, so BANK_SEL + BANK_IDX never leaves page 0x68.
; ============================================================
SEL_BANK:
            ldi     xh,>BANK_SEL
            lda     (BANK_IDX)
            sta     xl
            ldi     a,0x00
            sta     (x)
            rtn

; SEL_BANK0 -- return to bank 0 iff any bank switching was done.
SEL_BANK0:
            lda     (BANK_COUNT)
            bzs     SB0_DONE
            ldi     xh,>BANK_SEL
            ldi     xl,0x00
            ldi     a,0x00
            sta     (x)
SB0_DONE:
            rtn

; ============================================================
; FILL -- write uh to every byte of [SWP_START_H:00, SWP_END_H:00).
; ============================================================
FILL:
            lda     (SWP_START_H)
            sta     xh
            ldi     xl,0x00
            lda     (SWP_END_H)
            sta     yh
FILL_LOOP:
            lda     uh
            sin     x
            lda     xh
            cpa     yh
            bcr     FILL_LOOP
            rtn

; ============================================================
; VERIFY -- read every byte of [SWP_START_H:00, SWP_END_H:00); each
; must equal uh. First mismatch: record it (ERR_FLAG=4) and jump to
; BUILD_RESULT. Never returns on mismatch.
; ============================================================
VERIFY:
            lda     (SWP_START_H)
            sta     xh
            ldi     xl,0x00
            lda     (SWP_END_H)
            sta     yh
VFY_LOOP:
            lda     (x)
            cpa     uh
            bzr     VFY_BAD
            inc     x
            lda     xh
            cpa     yh
            bcr     VFY_LOOP
            rtn
VFY_BAD:
            sta     (ERR_ACTUAL)
            lda     uh
            sta     (ERR_EXPCT)
            lda     xh
            sta     (ERR_ADDR_H)
            lda     xl
            sta     (ERR_ADDR_L)
            lda     (BANK_IDX)
            sta     (BANK_AT_ERR)
            ldi     a,0x04
            sta     (ERR_FLAG)
            sjp     SEL_BANK0
            pop     x                  ; drop VERIFY's stranded return addr
            jmp     BUILD_RESULT

; ============================================================
; Return paths -- each sets X/A per the string-CALL convention and
; returns with carry set.
; ============================================================
RETURN_OK:
            ldi     xh,>MSG_OK
            ldi     xl,<MSG_OK
            ldi     a,0x07
            sec
            rtn

RETURN_NOTRUN:
            ldi     xh,>MSG_NOTRUN
            ldi     xl,<MSG_NOTRUN
            ldi     a,0x07
            sec
            rtn

RETURN_BREAK:
            ldi     xh,>MSG_BREAK
            ldi     xl,<MSG_BREAK
            ldi     a,0x05
            sec
            rtn

; ============================================================
; BUILD_RESULT -- assemble the result string in RESULT_BUF from
; ERR_FLAG (1 -> "Fail B..", 4 -> "Bank mismatch: .."), BANK_AT_ERR
; and ERR_ADDR_H/L, then return it (X=buf, A=len, carry set).
; ============================================================
BUILD_RESULT:
            ldi     xh,>RESULT_BUF
            ldi     xl,<RESULT_BUF
            lda     xl
            sta     (LEN_BASE)         ; buf-start low byte, for the length calc

            lda     (ERR_FLAG)
            cpi     a,0x04
            bzs     BR_MISMATCH

; ---- "Fail B" <bank> " &" <HHHH> ----
            ldi     yh,>MSG_FAIL
            ldi     yl,<MSG_FAIL
            ldi     a,0x06
            sjp     BR_COPY
            lda     (BANK_AT_ERR)     ; bank index is a single digit (0-9)
            rec
            adi     a,0x30
            sin     x
            ldi     a,0x20             ; ' '
            sin     x
            ldi     a,0x26             ; '&'
            sin     x
            lda     (ERR_ADDR_H)
            sjp     BR_HEX2
            lda     (ERR_ADDR_L)
            sjp     BR_HEX2
            bch     BR_FINISH

; ---- "Bank mismatch: " <bank> ----
BR_MISMATCH:
            ldi     yh,>MSG_MISM
            ldi     yl,<MSG_MISM
            ldi     a,0x0F            ; 15
            sjp     BR_COPY
            lda     (BANK_AT_ERR)     ; bank index is a single digit (0-9)
            rec
            adi     a,0x30
            sin     x

BR_FINISH:
            lda     xl
            sec
            sbc     (LEN_BASE)         ; A = bytes written = string length
            sta     (LEN_BASE)
            ldi     xh,>RESULT_BUF
            ldi     xl,<RESULT_BUF
            lda     (LEN_BASE)
            sec
            rtn

; BR_COPY -- copy A bytes from (Y) to (X), advancing both.
BR_COPY:
            sta     (PSTR_COUNT)
BR_COPY_L:
            lda     (PSTR_COUNT)
            bzs     BR_COPY_DONE
            lin     y
            sin     x
            lda     (PSTR_COUNT)
            dec     a
            sta     (PSTR_COUNT)
            bch     BR_COPY_L
BR_COPY_DONE:
            rtn

; BR_HEX2 -- append A as two hex digits at (X); x advances.
BR_HEX2:
            sta     (TEMP_DIGIT)
            shr
            shr
            shr
            shr
            sjp     NIB2HEX
            sin     x
            lda     (TEMP_DIGIT)
            ani     a,0x0F
            sjp     NIB2HEX
            sin     x
            rtn

; NIB2HEX -- A = nibble (0-15) -> ASCII hex digit in A.
NIB2HEX:
            cpi     a,0x0A
            bcr     NIB2HEX_LOW
            rec
            adi     a,0x37             ; 10->'A' .. 15->'F'
            rtn
NIB2HEX_LOW:
            rec
            adi     a,0x30             ; 0->'0' .. 9->'9'
            rtn

; PASS_COUNT = PASS_COUNT*10 + TEMP_DIGIT (no MUL -- old*8 + old*2).
MUL10ADD:
            lda     (PASS_COUNT)
            sta     ul
            shl
            sta     (TEMP16)
            lda     ul
            shl
            shl
            shl
            rec
            adc     (TEMP16)
            rec
            adc     (TEMP_DIGIT)
            sta     (PASS_COUNT)
            rtn

; ============================================================
; DISPLAY_STATUS -- print the progress line via the ROM LCD routines.
;   DISP_SHOWBANK = 0 : "Pass: " <value>
;   DISP_SHOWBANK = 1 : "Bank: " <bank> " / Pass: " <value>
;   <value> is PASS_IDX (decimal) when DISP_KIND = 0,
;           "Mark" when DISP_KIND = 1, "Check" when DISP_KIND = 2.
; Saves/restores Y and U (PRINT_CHAR's vmj 0x8A clobbers both).
; ============================================================
DISPLAY_STATUS:
            psh     y
            psh     u

            vmj     0xF2                ; clear LCD
            ldi     a,0x00
            sta     (CURSOR_PTR)        ; home cursor

            lda     (DISP_SHOWBANK)
            bzs     DS_PASSLBL

            ldi     xh,>MSG_BANK
            ldi     xl,<MSG_BANK
            ldi     a,0x06
            sjp     PRINT_STR
            lda     (BANK_IDX)
            sjp     PRINT_BYTE_DEC
            ldi     xh,>MSG_PASS
            ldi     xl,<MSG_PASS
            ldi     a,0x09
            sjp     PRINT_STR
            bch     DS_VALUE

DS_PASSLBL:
            ldi     xh,>MSG_PASS        ; "Pass: " == MSG_PASS + 3 (skip " / ")
            ldi     xl,<MSG_PASS
            inc     x
            inc     x
            inc     x
            ldi     a,0x06
            sjp     PRINT_STR

DS_VALUE:
            lda     (DISP_KIND)
            bzs     DS_NUM
            cpi     a,0x01
            bzs     DS_MARK
            ldi     xh,>MSG_CHECK
            ldi     xl,<MSG_CHECK
            ldi     a,0x05
            sjp     PRINT_STR
            bch     DS_END
DS_MARK:
            ldi     xh,>MSG_MARK
            ldi     xl,<MSG_MARK
            ldi     a,0x04
            sjp     PRINT_STR
            bch     DS_END
DS_NUM:
            lda     (PASS_IDX)
            sjp     PRINT_BYTE_DEC
DS_END:
            pop     u
            pop     y
            rtn

; Print A bytes starting at X. Walks the source via Y (PRINT_CHAR
; clobbers X to address the LCD, so Y is what survives each character).
PRINT_STR:
            sta     (PSTR_COUNT)
            lda     xh
            sta     yh
            lda     xl
            sta     yl
PSTR_LOOP:
            lda     (PSTR_COUNT)
            cpi     a,0x00
            bzs     PSTR_DONE
            lda     (y)
            sjp     PRINT_CHAR
            inc     y
            lda     (PSTR_COUNT)
            dec     a
            sta     (PSTR_COUNT)
            bch     PSTR_LOOP
PSTR_DONE:
            rtn

; Print the character in A at the cursor column, advance one glyph cell.
PRINT_CHAR:
            sta     (PCHAR_TMP)
            vmj     0x8C                ; X = LCD address for cursor column
            lda     (PCHAR_TMP)         ; vmj 0x8C clobbers A -- reload char
            vmj     0x8A                ; draw glyph
            vmj     0x8E
            vmj     0x8E
            vmj     0x8E
            vmj     0x8E
            vmj     0x8E
            vmj     0x8E
            rtn

; Print A as decimal, no leading zero. Values shown here are the bank
; index (0-9) and the pass counter (small); anything above 99 wraps
; mod 100 -- display only, the test itself still runs the full count.
PRINT_BYTE_DEC:
            sta     (REMAIN)
            ldi     a,0x00
            sta     (DIGIT_TMP)
PBD_TENS:   lda     (REMAIN)
            cpi     a,10
            bcr     PBD_TDONE
            sec
            sbi     a,10
            sta     (REMAIN)
            lda     (DIGIT_TMP)
            inc     a
            sta     (DIGIT_TMP)
            bch     PBD_TENS
PBD_TDONE:
            lda     (DIGIT_TMP)
            bzs     PBD_UNITS          ; no tens -> suppress leading zero
            rec
            adi     a,0x30
            sjp     PRINT_CHAR
PBD_UNITS:
            lda     (REMAIN)
            rec
            adi     a,0x30
            sjp     PRINT_CHAR
            rtn

; ============================================================
; Data -- placed after the code so it never sits in the fall-through
; path. RESULT_BUF is first so it stays well clear of the 0x7FFF page
; edge (BUILD_RESULT's length calc assumes it does not wrap a page).
; ============================================================
RESULT_BUF:     .db     0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
                .db     0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
                .db     0x00

ALLOC_SIZE:     .db     0x00
SCAN_LEFT:      .db     0x00
PASS_COUNT:     .db     0x00
BANK_COUNT:     .db     0x00
BANK_IDX:       .db     0x00
PASS_IDX:       .db     0x00
PHASE:          .db     0x00
PAT_IDX:        .db     0x00
TEMP_DIGIT:     .db     0x00
TEMP16:         .db     0x00
DIGIT_TMP:      .db     0x00
PCHAR_TMP:      .db     0x00
PSTR_COUNT:     .db     0x00
REMAIN:         .db     0x00
SWP_START_H:    .db     0x00
SWP_END_H:      .db     0x00
DISP_SHOWBANK:  .db     0x00
DISP_KIND:      .db     0x00
LEN_BASE:       .db     0x00

PAT_TBL:        .db     0x55,0xAA,0xFF,0x00

MSG_OK:         .ascii  "Test ok"
MSG_NOTRUN:     .ascii  "Not run"
MSG_BREAK:      .ascii  "Break"
MSG_BANK:       .ascii  "Bank: "
MSG_PASS:       .ascii  " / Pass: "
MSG_MARK:       .ascii  "Mark"
MSG_CHECK:      .ascii  "Check"
MSG_FAIL:       .ascii  "Fail B"
MSG_MISM:       .ascii  "Bank mismatch: "
