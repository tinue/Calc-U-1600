; {{name}}.asm -- RENUM for the PC-1500: a ROM extension (LH5801,
; sdaslh5801) that adds a BASIC command, plugged in at &8800 by
; debug.pc1500a's bus-rom:.
;
; Debugging it: F5 assembles the ROM, starts the machine with it, enters
; debug.pc1500a's sample program and types its `command:` (RENUM 100,,10).
; Set a breakpoint first, on RENUM, RENSCAN, RENFIX or RENNUMBER, or on
; any line below. Build & Load (Cmd+Alt+L / Ctrl+Alt+L) rebuilds it and
; sets the machine up again. `boot: debug` in debug.pc1500a stops at INIT
; instead, the code the firmware calls at power-on.
;
;   RENUM [new][,[old][,step]]
;
; renumbers the program in memory, as the PC-1600's built-in RENUM does:
; the lines from line `old` on (default: the first line) get the numbers
; new, new+step, ... (defaults 10 and 10), and every GOTO, GOSUB, THEN,
; ON...GOTO/GOSUB and RESTORE that names one of them follows. RENUM works
; in RUN and PRO mode; inside a program it is ERROR 1. On any error the
; program is left as it was:
;   ERROR 19  new or step is 0 or above 65279, or the new numbers would
;             reach the line before `old`
;   ERROR 11  line `old` doesn't exist
;   ERROR 30  the last new number would be above 65279
;   ERROR 13  the longer line numbers don't fit in memory
;   ERROR 11 IN n  line n jumps to a line that doesn't exist
;   ERROR 1 IN n   line n jumps to a computed line (GOTO A, GOTO 100+X)
; RENUM refuses a merged program (MERGE) with ERROR 1.
;
; How the firmware finds the command
; ----------------------------------
; At power-on and for every word it doesn't know, the PC-1500 ROM looks at
; the 2 KB pages &8000-&B800, with PV low and high, for a keyword module
; (ROM A04: TOK_TABL_SRCH E4A8, the name lookup at F9C7, TOK_PROCESS FA89):
;   +00   55H, the module's sentinel
;   +0A   code the ROM calls at power-on (RESET_19 E10B), ending in RTN
;   +20   an index: one word per letter A-Z, the address of the 2nd letter
;         of the first keyword with that letter, or 0
;   +54   the keyword table, entries of
;         marker, name, code (2 bytes), address (2 bytes)
;         - the marker's low nibble is the name's length, 0 ends the table;
;         - its high nibble describes the keyword BEFORE it (as in the
;           ROM's own table at C054, CNIB macro): bit 4 = last keyword
;           with that letter, bits 6-5 = where it may run: 00 typed only
;           (the routine's first byte is then a mask of the allowed
;           modes, ANDed with 764FH: 40H RUN, 20H PRO, 10H RESERVE, and
;           execution starts after it), 01 in a program only, 1x both;
;         - the code's high byte E0H-EFH names the page: 80H+(code&7)*8,
;           PV high for E8H-EFH; a low byte of 80H or more is a statement.
; When the command runs, Y points just past its token at the rest of the
; statement, and the routine ends with VEJ (E2), the ROM's "next statement".
;
; How RENUM works
; ---------------
; The algorithm follows the PC-1600's RENUM (ROM P1-B3B, K_RENUM 45BDH):
; check the arguments, collect the references, renumber, fix the
; references. The PC-1600 keeps a line reference in binary and patches it
; in place; the PC-1500 keeps it as decimal digits, so a reference can get
; longer or shorter and the rest of the program moves. Hence two passes
; over the references:
;   1. RENSCAN (dry run): find every reference and its target, report the
;      first bad one, add up how much the program grows;
;   2. RENFIX: rewrite the references, moving the program as needed;
; then RENNUMBER writes the new line numbers. The references are fixed
; while the lines still have their old numbers, so finding a target is a
; plain search; no table is needed.
;
; A program line is: line number (2 bytes, high first), length (the bytes
; that follow, up to and including the 0DH), the tokenized text, 0DH. The
; program ends with FFH, at BASPRG_END.

; ── ROM addresses (the same in A01, A03 and A04) ──────────────────────

RAM_END_H   .equ    0x7864      ; first page past the RAM
BASPRG_ST   .equ    0x7865      ; program start
BASPRG_END  .equ    0x7867      ; program end (its FFH)
BASPRG_EDT  .equ    0x7869      ; start of the last MERGEd program
CURR_LINE   .equ    0x789C      ; line number being executed (0: none)
CURR_TOP    .equ    0x789E      ; start of the program it is in
INIT_SYS    .equ    0xCFD0      ; reset DATA, GOSUB/FOR, ON ERROR, CONT
DEL_DIM_VAR .equ    0xD09C      ; drop arrays the program has grown into

TOK_GOTO    .equ    0x92        ; F1xxH tokens that take line numbers
TOK_GOSUB   .equ    0x94
TOK_RESTORE .equ    0xA7
TOK_REM     .equ    0xAB
TOK_THEN    .equ    0xAE

; ── Work variables: the string buffer (7B10H-7B5FH), free while RENUM
;    runs; 16-bit values high byte first, like the ROM's ───────────────

W           .equ    0x7B10
R_NEW       .equ    W+0         ; first new line number
R_OLD       .equ    W+2         ; first line to renumber (0: the first)
R_STEP      .equ    W+4         ; increment
R_OADR      .equ    W+6         ; address of line `old`
R_PREV      .equ    W+8         ; number of the line before it (0: none)
R_VAL       .equ    W+10        ; running new number
R_LINE      .equ    W+12        ; line being scanned
R_PTR       .equ    W+14        ; start of the reference's digits
R_END       .equ    W+16        ; just past them
R_GROW      .equ    W+18        ; how much the program grows (signed)
R_T         .equ    W+20        ; scratch word
R_Y         .equ    W+22        ; the statement pointer to return with
R_PASS      .equ    W+24        ; 0 dry run, 1 fix
R_INR       .equ    W+25        ; NEWNUM: in the renumbered range
R_OVF       .equ    W+26        ; reference too big to be a line number
R_OLEN      .equ    W+27        ; digits in the reference now
R_NLEN      .equ    W+28        ; digits it will have
R_DLT       .equ    W+29        ; R_NLEN - R_OLEN
R_DGT       .equ    W+30        ; TODEC: current digit
R_LEAD      .equ    W+31        ; TODEC: a digit has been written
R_CNT       .equ    W+32        ; TODEC: powers left
R_DIGIT     .equ    W+33        ; parse: digit being added
R_DIG       .equ    W+34        ; the new digits (5)

            .area   CODE (ABS)

; ── The module header ─────────────────────────────────────────────────

            .org    0x8800
            .db     0x55        ; sentinel: a keyword module lives here

            .org    0x880A
INIT:       rtn                 ; nothing to set up at power-on

            .org    0x8820      ; the index, A-Z
            .dw     0, 0, 0, 0, 0, 0, 0, 0, 0       ; A-I
            .dw     0, 0, 0, 0, 0, 0, 0, 0          ; J-Q
            .dw     KW_RENUM+2                      ; R: the E of RENUM
            .dw     0, 0, 0, 0, 0, 0, 0, 0          ; S-Z

            .org    0x8854      ; the keyword table
KW_RENUM:   .db     0xC5        ; length 5 (high nibble: no keyword before)
            .ascii  "RENUM"
            .dw     0xE180      ; code: page 8800H (E1), PV low; a statement
            .dw     RENUM_MODES ; address
            .db     0x90        ; end of table; for RENUM: last R keyword,
                                ; typed only (bits 6-5 = 00)

; ── RENUM ─────────────────────────────────────────────────────────────

RENUM_MODES: .db    0x60        ; allowed in RUN and PRO mode; RENUM follows

; Arguments: [new][,[old][,step]], each an expression. The values go on
; the stack; the work variables are only safe once no expression is
; being evaluated.
RENUM:      ldi     uh,0
            ldi     ul,10
            sjp     GETNUM      ; new
            psh     u
            ldi     uh,0
            ldi     ul,0
            lda     (y)
            cpi     a,0x2C      ; ','
            bzr     ARG_OLD
            inc     y
            sjp     GETNUM      ; old
ARG_OLD:    psh     u
            ldi     uh,0
            ldi     ul,10
            lda     (y)
            cpi     a,0x2C
            bzr     ARG_STEP
            inc     y
            sjp     GETNUM      ; step
ARG_STEP:   psh     u
            lda     (y)
            cpi     a,0x0D      ; nothing may follow
            bzs     ARG_DONE
            vej     0xE4        ; ERROR 1
ARG_DONE:   pop     u
            lda     uh
            sta     (R_STEP)
            lda     ul
            sta     (R_STEP+1)
            pop     u
            lda     uh
            sta     (R_OLD)
            lda     ul
            sta     (R_OLD+1)
            pop     u
            lda     uh
            sta     (R_NEW)
            lda     ul
            sta     (R_NEW+1)
            lda     yh
            sta     (R_Y)
            lda     yl
            sta     (R_Y+1)

; new and step: 1..65279
            ldi     uh,19
            lda     (R_NEW)
            cpi     a,0xFF
            bzs     ERROR
            ora     (R_NEW+1)
            bzs     ERROR
            lda     (R_STEP)
            cpi     a,0xFF
            bzs     ERROR
            ora     (R_STEP+1)
            bzs     ERROR

; One program only: after a MERGE, BASPRG_EDT points at the last one.
            ldi     uh,1
            lda     (BASPRG_EDT)
            cpa     (BASPRG_ST)
            bzr     ERROR
            lda     (BASPRG_EDT+1)
            cpa     (BASPRG_ST+1)
            bzr     ERROR

; An empty program: nothing to do.
            sjp     PRGSTART
            lda     (x)
            cpi     a,0xFF
            bzr     FINDOLD
            jmp     FINISH

ERROR:      vej     0xE0        ; ERROR UH

; Find line `old` (R_OADR) and the number of the line before it (R_PREV).
FINDOLD:    ldi     a,0
            sta     (R_PREV)
            sta     (R_PREV+1)
            lda     (R_OLD)
            ora     (R_OLD+1)
            bzs     FO_FOUND    ; old = 0: the first line
FO_LOOP:    lda     (x)
            cpi     a,0xFF
            bzs     FO_MISS
            cpa     (R_OLD)
            bzr     FO_NEXT
            inc     x
            lda     (x)
            dec     x
            cpa     (R_OLD+1)
            bzs     FO_FOUND
FO_NEXT:    lda     (x)
            sta     (R_PREV)
            inc     x
            lda     (x)
            sta     (R_PREV+1)
            dec     x
            sjp     NEXTLINE
            bch     FO_LOOP
FO_MISS:    ldi     uh,11
            bch     ERROR
FO_FOUND:   lda     xh
            sta     (R_OADR)
            lda     xl
            sta     (R_OADR+1)

; The new numbers must stay above the line before `old` ...
            ldi     uh,19
            sec
            lda     (R_PREV+1)
            sbc     (R_NEW+1)
            lda     (R_PREV)
            sbc     (R_NEW)
            bcs     ERROR       ; prev >= new

; ... and below 65280.
            ldi     uh,30
            sjp     STARTVAL
CR_LOOP:    sjp     NEXTLINE
            lda     (x)
            cpi     a,0xFF
            bzs     CR_DONE
            sjp     STEPVAL
            bcs     ERROR       ; carry out of 16 bits
            lda     (R_VAL)
            cpi     a,0xFF
            bzs     ERROR
            bch     CR_LOOP
CR_DONE:

; Pass 1, dry run: check every reference, add up the growth.
RENSCAN:    ldi     a,0
            sta     (R_PASS)
            sta     (R_GROW)
            sta     (R_GROW+1)
            sjp     SCANREFS

; Room for the growth? Like the ROM's own line insert (PRGLINE_TDI CF27):
; the new end must stay below RAM_END_H.
            lda     (R_GROW)
            bii     a,0x80
            bzr     MEM_OK      ; the program shrinks
            rec
            lda     (BASPRG_END+1)
            adc     (R_GROW+1)
            lda     (BASPRG_END)
            adc     (R_GROW)
            ldi     uh,13
            bcs     ERROR
            cpa     (RAM_END_H)
            bcs     ERROR
MEM_OK:

; Pass 2: rewrite the references.
RENFIX:     ldi     a,1
            sta     (R_PASS)
            sjp     SCANREFS

; The new line numbers, from line `old` on.
RENNUMBER:  sjp     STARTVAL
RN_LOOP:    lda     (x)
            cpi     a,0xFF
            bzs     RN_DONE
            lda     (R_VAL)
            sin     x
            lda     (R_VAL+1)
            sin     x
            sjp     NEXTLEN
            sjp     STEPVAL
            bch     RN_LOOP
RN_DONE:
            sjp     DEL_DIM_VAR ; as after a line edit
            sjp     INIT_SYS    ; nothing may point into the old program

FINISH:     lda     (R_Y)
            sta     yh
            lda     (R_Y+1)
            sta     yl
            vej     0xE2        ; end of statement

; ── Argument parsing ──────────────────────────────────────────────────

; GETNUM: U = the number at Y, or U unchanged (the default) if the
; argument is left out (Y at ',' or the end).
GETNUM:     lda     (y)
            cpi     a,0x2C
            bzs     GN_RET
            cpi     a,0x0D
            bzs     GN_RET
            vej     0xDE        ; evaluate the expression at Y
            .db     GN_ERR-.-1
            vej     0xD0        ; to a 16-bit integer in U
            .db     0x00
            .db     GN_ERR-.-1
GN_RET:     rtn
GN_ERR:     vej     0xE0

; ── Walking the program ───────────────────────────────────────────────

; PRGSTART: X = the first line.
PRGSTART:   lda     (BASPRG_ST)
            sta     xh
            lda     (BASPRG_ST+1)
            sta     xl
            rtn

; NEXTLINE: X from a line's start to the next line's.
NEXTLINE:   inc     x
            inc     x
; NEXTLEN: the same from the line's length byte.
NEXTLEN:    lin     x
            adr     x
            rtn

; STARTVAL: X = line `old`, R_VAL = new.
STARTVAL:   lda     (R_OADR)
            sta     xh
            lda     (R_OADR+1)
            sta     xl
            lda     (R_NEW)
            sta     (R_VAL)
            lda     (R_NEW+1)
            sta     (R_VAL+1)
            rtn

; STEPVAL: R_VAL += step; C = carry out.
STEPVAL:    rec
            lda     (R_VAL+1)
            adc     (R_STEP+1)
            sta     (R_VAL+1)
            lda     (R_VAL)
            adc     (R_STEP)
            sta     (R_VAL)
            rtn

; ── Every line reference in the program (RENSCAN, RENFIX) ─────────────

SCANREFS:   sjp     PRGSTART
SC_LINE:    lda     (x)
            cpi     a,0xFF
            bzr     SC_BODY
            rtn
SC_BODY:    lda     xh
            sta     (R_LINE)
            lda     xl
            sta     (R_LINE+1)
            inc     x
            inc     x
            inc     x
SC_ELEM:    lda     (x)
            cpi     a,0x0D
            bzs     SC_NEXT
            cpi     a,0x22      ; '"': skip the string
            bzr     SC_NOSTR
            sjp     SKIPSTR
            bch     SC_ELEM
SC_NOSTR:   cpi     a,0xE0
            bcs     SC_TOKEN
            inc     x           ; a character
            bch     SC_ELEM
SC_TOKEN:   inc     x
            cpi     a,0xF1      ; BASIC's own statements are F1xxH
            bzs     SC_F1
            inc     x           ; another token: skip its low byte
            bch     SC_ELEM
SC_F1:      lin     x           ; A = the token's low byte
            cpi     a,TOK_REM   ; REM: the rest of the line is text
            bzs     SC_REM
            cpi     a,TOK_GOTO
            bzs     SC_REFS
            cpi     a,TOK_GOSUB
            bzs     SC_REFS
            cpi     a,TOK_RESTORE
            bzs     SC_REFS
            cpi     a,TOK_THEN  ; THEN 100, but not THEN PRINT
            bzr     SC_ELEM
            lda     (x)
            sjp     ISDIGIT
            bcr     SC_ELEM
SC_REFS:    sjp     REFLIST
            bch     SC_ELEM
SC_REM:     lda     (x)
            cpi     a,0x0D
            bzs     SC_NEXT
            inc     x
            bch     SC_REM
SC_NEXT:    inc     x           ; past the 0DH: the next line
            bch     SC_LINE

; SKIPSTR: X from a '"' to just past the closing one (or to the 0DH).
SKIPSTR:    inc     x
SS_LOOP:    lda     (x)
            cpi     a,0x0D
            bzs     SS_RET
            inc     x
            cpi     a,0x22
            bzr     SS_LOOP
SS_RET:     rtn

; ISDIGIT: C = 1 if A is '0'-'9'. A is kept.
ISDIGIT:    cpi     a,0x3A
            bcs     ISD_NO
            cpi     a,0x30
            rtn
ISD_NO:     rec
            rtn

; REFLIST: the references after GOTO/GOSUB/RESTORE/THEN: a number or a
; label ("A"), several for ON ... GOTO, or none (RESTORE).
REFLIST:    lda     (x)
            cpi     a,0x22
            bzr     RL_NUM
            sjp     SKIPSTR     ; a label: leave it
            bch     RL_SEP
RL_NUM:     sjp     ISDIGIT
            bcr     RL_END      ; no number here
            sjp     DOREF
RL_SEP:     lda     (x)
            cpi     a,0x2C
            bzr     RL_END
            inc     x
            bch     REFLIST
RL_END:     cpi     a,0x0D      ; the statement must end here
            bzs     RL_RET
            cpi     a,0x3A      ; ':'
            bzs     RL_RET
            ldi     uh,1        ; anything else is a computed line
            jmp     ERR_IN
RL_RET:     rtn

; DOREF: the reference at X. Dry run: look it up, add the growth.
; Fix: rewrite it. Returns X past it.
DOREF:      lda     xh
            sta     (R_PTR)
            lda     xl
            sta     (R_PTR+1)
            ldi     a,0
            sta     (R_OVF)
            sta     (R_OLEN)
            ldi     uh,0
            ldi     ul,0
DR_DIGIT:   lda     (x)
            sjp     ISDIGIT
            bcr     DR_GOT
            ani     a,0x0F
            sta     (R_DIGIT)
            sjp     MUL10ADD
            inc     x
            lda     (R_OLEN)
            inc     a
            sta     (R_OLEN)
            bch     DR_DIGIT
DR_GOT:     lda     xh
            sta     (R_END)
            lda     xl
            sta     (R_END+1)
            lda     (R_OVF)
            bzr     DR_UNDEF
            sjp     NEWNUM
            bcr     DR_UNDEF
            sjp     TODEC
            sec
            lda     (R_NLEN)
            sbc     (R_OLEN)
            sta     (R_DLT)
            lda     (R_PASS)
            bzr     DR_FIX
            ldi     xh,>R_GROW  ; dry run: R_GROW += delta
            ldi     xl,<R_GROW
            lda     (R_DLT)
            sjp     ADDS16
            lda     (R_END)
            sta     xh
            lda     (R_END+1)
            sta     xl
            rtn
DR_UNDEF:   ldi     uh,11
            jmp     ERR_IN
DR_FIX:     lda     (R_DLT)
            bzs     DR_WRITE
            sjp     MOVE
DR_WRITE:   lda     (R_PTR)     ; the new digits over the old
            sta     yh
            lda     (R_PTR+1)
            sta     yl
            ldi     xh,>R_DIG
            ldi     xl,<R_DIG
            lda     (R_NLEN)
            dec     a
            sta     ul
DR_COPY:    tin
            lop     ul,DR_COPY
            ldx     y           ; X past them
            rtn

; MUL10ADD: U = U*10 + R_DIGIT; R_OVF = 1 past 16 bits.
MUL10ADD:   sjp     SHLU        ; U*2
            lda     uh
            sta     (R_T)
            lda     ul
            sta     (R_T+1)
            sjp     SHLU
            sjp     SHLU        ; U*8
            rec
            lda     ul
            adc     (R_T+1)
            sta     ul
            lda     uh
            adc     (R_T)       ; U*10
            sta     uh
            sjp     OVFC
            rec
            lda     ul
            adc     (R_DIGIT)
            sta     ul
            lda     uh
            adi     a,0         ; + carry
            sta     uh
            bch     OVFC
SHLU:       lda     ul
            shl
            sta     ul
            lda     uh
            rol
            sta     uh
OVFC:       bcr     OV_RET
            ldi     a,1
            sta     (R_OVF)
OV_RET:     rtn

; NEWNUM: U = a line number. If that line exists: C = 1 and U = its new
; number (unchanged before line `old`). Otherwise C = 0.
NEWNUM:     sjp     PRGSTART
            ldi     a,0
            sta     (R_INR)
NN_LINE:    lda     xh          ; at line `old`, the new numbers start
            cpa     (R_OADR)
            bzr     NN_CHECK
            lda     xl
            cpa     (R_OADR+1)
            bzr     NN_CHECK
            lda     (R_NEW)
            sta     (R_VAL)
            lda     (R_NEW+1)
            sta     (R_VAL+1)
            ldi     a,1
            sta     (R_INR)
NN_CHECK:   lda     (x)
            cpi     a,0xFF
            bzs     NN_MISS
            cpa     uh
            bzr     NN_NEXT
            inc     x
            lda     (x)
            dec     x
            cpa     ul
            bzr     NN_NEXT
            lda     (R_INR)     ; found
            bzs     NN_HIT
            lda     (R_VAL)
            sta     uh
            lda     (R_VAL+1)
            sta     ul
NN_HIT:     sec
            rtn
NN_NEXT:    lda     (R_INR)
            bzs     NN_ADV
            sjp     STEPVAL
NN_ADV:     sjp     NEXTLINE
            bch     NN_LINE
NN_MISS:    rec
            rtn

; TODEC: U (1..65279) to decimal digits at R_DIG, R_NLEN of them.
TODEC:      ldi     yh,>R_DIG
            ldi     yl,<R_DIG
            ldi     xh,>POWERS
            ldi     xl,<POWERS
            ldi     a,0
            sta     (R_LEAD)
            ldi     a,4
            sta     (R_CNT)
TD_POW:     ldi     a,0
            sta     (R_DGT)
TD_SUB:     sec                 ; U -= power while it fits
            lda     ul
            sbc     (x)
            sta     (R_T)
            inc     x
            lda     uh
            sbc     (x)
            dec     x
            bcr     TD_STOP
            sta     uh
            lda     (R_T)
            sta     ul
            lda     (R_DGT)
            inc     a
            sta     (R_DGT)
            bch     TD_SUB
TD_STOP:    lda     (R_DGT)
            bzr     TD_PUT
            lda     (R_LEAD)    ; no leading zeros
            bzs     TD_NEXT
            lda     (R_DGT)
TD_PUT:     ori     a,0x30
            sin     y
            ldi     a,1
            sta     (R_LEAD)
TD_NEXT:    inc     x
            inc     x
            lda     (R_CNT)
            dec     a
            sta     (R_CNT)
            bzr     TD_POW
            lda     ul          ; the units
            ori     a,0x30
            sin     y
            lda     yl
            sec
            sbi     a,<R_DIG
            sta     (R_NLEN)
            rtn

POWERS:     .db     <10000, >10000
            .db     <1000, >1000
            .db     <100, >100
            .db     <10, >10

; MOVE: make room for (R_DLT > 0) or close the gap of (R_DLT < 0) the
; new digits: everything from R_END to the program's end moves by R_DLT.
; Then the line's length, BASPRG_END and R_OADR follow.
MOVE:       sec                 ; U = END - R_END: bytes to move - 1
            lda     (BASPRG_END+1)
            sbc     (R_END+1)
            sta     ul
            lda     (BASPRG_END)
            sbc     (R_END)
            sta     uh
            lda     (R_DLT)
            bii     a,0x80
            bzr     MV_DOWN
            lda     (BASPRG_END) ; up: copy from the end down
            sta     xh
            lda     (BASPRG_END+1)
            sta     xl
            stx     y
            lda     (R_DLT)
            adr     y
MV_UP:      lde     x
            sde     y
            lop     ul,MV_UP
            dec     uh
            bcs     MV_UP
            bch     MV_FIX
MV_DOWN:    lda     (R_END)     ; down: copy from the start up
            sta     xh
            lda     (R_END+1)
            sta     xl
            lda     (R_PTR)
            sta     yh
            lda     (R_PTR+1)
            sta     yl
            lda     (R_NLEN)
            adr     y
MV_DN:      tin
            lop     ul,MV_DN
            dec     uh
            bcs     MV_DN
MV_FIX:     lda     (R_LINE)    ; the line's length byte
            sta     xh
            lda     (R_LINE+1)
            sta     xl
            inc     x
            inc     x
            rec
            lda     (x)
            adc     (R_DLT)
            sta     (x)
            ldi     xh,>BASPRG_END
            ldi     xl,<BASPRG_END
            lda     (R_DLT)
            sjp     ADDS16
            sec                 ; line `old` moves if it is further on
            lda     (R_PTR+1)
            sbc     (R_OADR+1)
            lda     (R_PTR)
            sbc     (R_OADR)
            bcs     MV_RET
            ldi     xh,>R_OADR
            ldi     xl,<R_OADR
            lda     (R_DLT)
            sjp     ADDS16
MV_RET:     rtn

; ADDS16: the 16-bit value at X += A (signed).
ADDS16:     sta     ul
            ldi     uh,0
            bii     a,0x80
            bzs     AS_POS
            ldi     uh,0xFF
AS_POS:     inc     x
            rec
            lda     (x)
            adc     ul
            sta     (x)
            dec     x
            lda     (x)
            adc     uh
            sta     (x)
            rtn

; ERR_IN: ERROR UH IN the line being scanned. The program is unchanged
; (errors come from the dry run); the ROM shows the error as one in that
; line, and the up arrow shows the line.
ERR_IN:     psh     u
            psh     x
            sjp     INIT_SYS    ; ON ERROR GOTO must not catch it
            pop     x
            pop     u
            stx     y           ; where in the line
            lda     (R_LINE)
            sta     xh
            lda     (R_LINE+1)
            sta     xl
            lda     (x)
            sta     (CURR_LINE)
            inc     x
            lda     (x)
            sta     (CURR_LINE+1)
            lda     (BASPRG_ST)
            sta     (CURR_TOP)
            lda     (BASPRG_ST+1)
            sta     (CURR_TOP+1)
            vej     0xE0
