; ============================================================
; MEMTEST.ASM -- Generic Free-Memory Test for Sharp PC-1500 / PC-1500A
; ============================================================
;
; Tests all currently-free, currently-installed RAM for faults: writes
; four patterns (0x55, 0xAA, 0xFF, 0x00) to every byte from BASIC's own
; free-memory pointer (BOTTOM_H/L, &7867/&7868) up to the ROM's own
; end-of-RAM pointer (RAM_END_H, &7864), reading each back and comparing.
; At the first mismatch the bad address and data are saved and the test
; stops; on a clean pass ERR_FLAG is 0. This clobbers any DIMmed
; variables/BASIC program in that range -- by design, not a bug.
;
; Because it always uses the machine's own reported free-RAM bounds, one
; binary works correctly whether a memory module is installed or not,
; and whatever its size -- the only thing that has to match the real
; hardware is where THIS CODE ITSELF gets loaded, since that's baked into
; the binary as absolute addresses (LH5801 code isn't
; position-independent). That address follows the usual convention:
; RAM_start + &C5 (the ROM reserves the first &C5 bytes of any RAM block
; for its own use) -- see "Build selection" below.
;
; Calling convention: X = iteration count on entry (1-255; 0 = skipped,
; ERR_FLAG=2). Returns with carry set, X = ERR_FLAG (0 = pass, 1 = fault,
; 2 = not run). ERR_FLAG is at ENTRY+2; on a fault, ERR_ADDR_H/L,
; ERR_EXPCT, ERR_ACTUAL (ENTRY+3..+6) hold the bad address and the
; expected/actual byte.
;
; Assembly command:
;   sdaslh5801 -plosgff memtest.asm
;   (or use the lh5801-asm VS Code extension's "LH5801: Assemble to BIN")
;
; --- Build selection: uncomment ONE ENTRY line, assemble, then PEEK
;     ENTRY+2..ENTRY+6 to read results ---
;
; PC-1500A -- machine language area, always present regardless of any
; module (see examples/memtest_1500a.pc1500a):
;   ENTRY = 0x7C01   NEW: none needed     RUN: CALL &7C01,X
;
; PC-1500 stock, no module -- built-in 2K RAM starts at &4000 (see
; examples/memtest_stock.pc1500):
;   ENTRY = 0x40C5   NEW &417D            RUN: CALL &40C5,X
;
; PC-1500 + CE-155 -- clamps around the built-in 2K (2K before it, 6K
; after), giving one contiguous &3800-&5FFF block (10K):
;   ENTRY = 0x38C5   NEW &397D            RUN: CALL &38C5,X
;
; PC-1500 + a 16K Y0 module -- &0000-&3FFF, contiguous with the built-in
; 2K that follows it at &4000-&47FF (18K total):
;   ENTRY = 0x00C5   NEW &017D            RUN: CALL &C5,X
;
; NEW's value above is ENTRY + this build's assembled size (184 bytes for
; all four -- ENTRY doesn't change the code's own length), i.e. the exact
; first free byte after the loaded stub, not a rounded-up guess.
;
; ============================================================

; --- Build selection: uncomment ONE line ---
ENTRY       .equ    0x40C5      ; PC-1500 stock, no module
;ENTRY      .equ    0x7C01      ; PC-1500A
;ENTRY      .equ    0x38C5      ; PC-1500 + CE-155
;ENTRY      .equ    0x00C5      ; PC-1500 + 16K Y0 module

BOTTOM_H    .equ    0x7867      ; High byte of BASIC's own free-memory pointer
BOTTOM_L    .equ    0x7868      ; Low byte  of BASIC's own free-memory pointer
RAM_END_H   .equ    0x7864      ; High byte of first invalid page (one past top of RAM)

; ============================================================
            .area   CODE (ABS)
            .org    ENTRY
; ============================================================

; +0  Two-byte forward branch; lands at MEMTEST (+7), skipping
;     the five result bytes.
            bch     MEMTEST     ; 8E 05

; +2  Result area -- read these with PEEK() after CALL
ERR_FLAG:   .db     0x00        ; 0 = all passes clean, 1 = fault found, 2 = not run
ERR_ADDR_H: .db     0x00        ; High byte of first bad address
ERR_ADDR_L: .db     0x00        ; Low byte  of first bad address
ERR_EXPCT:  .db     0x00        ; Pattern written   (expected value)
ERR_ACTUAL: .db     0x00        ; Value read back   (actual value)

; ============================================================
; MEMTEST -- main test routine, entered via CALL from BASIC
; ============================================================
MEMTEST:                        ; ENTRY+7

; Initialise: clear ERR_FLAG.
            ldi     a,0x00
            sta     (ERR_FLAG)

; Check whether the iteration count (X register on entry) is zero.
; CPA compares A with a register; A is still 0x00 from the LDI above.
            cpa     xh          ; flags = 0x00 - xh
            bzr     COUNT_OK    ; xh != 0 -> count is nonzero, proceed
            cpa     xl          ; xh==0, flags = 0x00 - xl
            bzr     COUNT_OK    ; xl != 0 -> count is nonzero, proceed

; Count is zero -- mark as "not run" and return.
            ldi     a,0x02
            sta     (ERR_FLAG)
            ldi     xh,0x00
            ldi     xl,0x02
            sec                 ; carry set -> BASIC writes X register back to variable
            rtn

COUNT_OK:
; Save the iteration count into UL, biased down by one: LOP branches
; back unless UL was already 0 *before* its decrement, so priming it
; with (count-1) makes the OUTER loop below run exactly `count` times.
; Supported range: 1-255 (XH is not used for the count).
            lda     xl
            sta     ul
            dec     ul

; Load Y with the end sentinel: one past the last valid byte, read from
; the ROM's own RAM_END_H pointer.
            lda     (RAM_END_H)
            sta     yh
            ldi     yl,0x00

; ============================================================
; OUTER -- one full sweep of all four patterns
; ============================================================
OUTER:
; ============================================================
; Pass 1 -- pattern 0x55 (01010101)
; ============================================================
PASS1:
            lda     (BOTTOM_H)          ; X = start of free memory
            sta     xh
            lda     (BOTTOM_L)
            sta     xl
            ldi     a,0x55
            sta     uh                  ; UH holds current pattern
LOOP1:      lda     uh
            sta     (x)                 ; write pattern
            lda     (x)                 ; read back
            cpa     uh                  ; compare (flags = a - uh)
            bzr     ERROR               ; Z=0 -> mismatch -> record & stop

            inc     x                   ; advance pointer

            lda     xh                  ; 16-bit end check: X vs Y
            cpa     yh                  ; flags = xh - yh
            bcr     LOOP1               ; C=0: xh < yh -> keep going
            bzr     NEXT1               ; C=1 and Z=0: xh > yh -> pass done (skip xl test)
            lda     xl
            cpa     yl                  ; flags = xl - yl
            bcr     LOOP1               ; C=0: xl < yl -> keep going
NEXT1:
; ============================================================
; Pass 2 -- pattern 0xAA (10101010)
; ============================================================
PASS2:
            lda     (BOTTOM_H)
            sta     xh
            lda     (BOTTOM_L)
            sta     xl
            ldi     a,0xAA
            sta     uh

LOOP2:      lda     uh
            sta     (x)
            lda     (x)
            cpa     uh
            bzr     ERROR

            inc     x

            lda     xh
            cpa     yh
            bcr     LOOP2
            bzr     NEXT2
            lda     xl
            cpa     yl
            bcr     LOOP2
NEXT2:

; ============================================================
; Pass 3 -- pattern 0xFF (11111111)
; ============================================================
PASS3:
            lda     (BOTTOM_H)
            sta     xh
            lda     (BOTTOM_L)
            sta     xl
            ldi     a,0xFF
            sta     uh

LOOP3:      lda     uh
            sta     (x)
            lda     (x)
            cpa     uh
            bzr     ERROR

            inc     x

            lda     xh
            cpa     yh
            bcr     LOOP3
            bzr     NEXT3
            lda     xl
            cpa     yl
            bcr     LOOP3
NEXT3:

; ============================================================
; Pass 4 -- pattern 0x00 (00000000)
; ============================================================
PASS4:
            lda     (BOTTOM_H)
            sta     xh
            lda     (BOTTOM_L)
            sta     xl
            ldi     a,0x00
            sta     uh

LOOP4:      lda     uh
            sta     (x)
            lda     (x)
            cpa     uh
            bzr     ERROR

            inc     x

            lda     xh
            cpa     yh
            bcr     LOOP4
            bzr     NEXT4
            lda     xl
            cpa     yl
            bcr     LOOP4
NEXT4:
; All four passes completed without error.
; LOP decrements UL and branches back to OUTER unless UL was already 0
; (see the priming comment at COUNT_OK above).
            lop     ul,OUTER

; ============================================================
; All iterations completed without error.
; ============================================================
            ldi     xh,0x00
            ldi     xl,0x00
            sec                         ; carry set -> BASIC writes result to variable
            rtn

; ============================================================
; ERROR -- record the first mismatch and return to BASIC
; ============================================================
; On entry: A = value actually read, X = bad address, UH = pattern written.
ERROR:
            sta     (ERR_ACTUAL)        ; save the bad readback
            lda     uh
            sta     (ERR_EXPCT)         ; save the expected pattern
            lda     xh
            sta     (ERR_ADDR_H)        ; save bad-address high byte
            lda     xl
            sta     (ERR_ADDR_L)        ; save bad-address low byte
            ldi     a,0x01
            sta     (ERR_FLAG)          ; mark failure
            ldi     xh,0x00
            ldi     xl,0x01
            sec                         ; carry set -> BASIC writes result to variable
            rtn
