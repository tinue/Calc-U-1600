; ============================================================
; INSTRQUIRKS_1500A.ASM -- Real-hardware verification of three
; LH5801 instructions implemented inconsistently across existing
; emulators (MESS/MAME, PockEmul, forever1500, this project), because
; the Sharp PC-1500 Technical Reference Manual is ambiguous or wrong
; about them.
;
; RESULT (confirmed on real PC-1500A hardware): reading (a) below --
; DRL -> A=0x34, mem=0x41 ; DRR -> A=0x34, mem=0x23 -- matching MESS/
; MAME, PockEmul, forever1500, and this project's own LH5801 core
; (Core/CPU/LH5801/LH5801.cpp's drlMerge/drrMerge). Readings (b) and
; (c) are both wrong; kept in the comments below only so a future
; reader can see what was ruled out and why the accumulator/memory
; pair was captured in the first place.
;
; Calc-U-1600's own ADR behaviour was already settled before this file
; existed (see the ADR section below and docs/Up-Down-Key-Investigation.md,
; "The ADR conflict"). This program exists so both findings can be
; reproduced independently on real hardware, by anyone, straight from
; PEEK.
;
; Target: PC-1500A, machine-language area (see examples/machine-code/memtest_1500a.pc1500a
; for why that area needs no NEW/module reservation on this model).
;
; ============================================================
; Test 1 & 2 -- DRL (X) / DRR (X), the nibble-rotate instructions
; ============================================================
; With A = 0x12 and (X) = 0x34, three readings existed in the wild
; before this test ran:
;
;   (a) MESS/MAME, PockEmul, forever1500: A is replaced by the ENTIRE
;       old memory byte.                                 -- CONFIRMED
;         DRL -> A=0x34, mem=0x41   |   DRR -> A=0x34, mem=0x23
;   (b) 3-nibble rotate, one nibble of A preserved.            -- WRONG
;         DRL -> A=0x32, mem=0x41   |   DRR -> A=0x14, mem=0x23
;   (c) Z80 RLD/RRD semantics (A's LOW nibble rotates, high nibble
;       preserved).                                          -- WRONG
;         DRL -> A=0x13, mem=0x42   |   DRR -> A=0x14, mem=0x23
;
; The accumulator alone discriminates all three readings for DRL; the
; memory byte then separates (c) from (a)/(b) (mem=0x42 vs 0x41). DRR's
; own memory result (0x23) is the same under all three readings, so it
; adds nothing beyond confirming DRL's verdict -- both A and mem are
; still captured for completeness and as a cross-check.
;
; Both tests use the unprefixed ME0 forms (opcodes 0xD7 / 0xD3, "drl
; (x)" / "drr (x)"), not the FD-prefixed ME1 forms, since the operand
; lives in main RAM, not the internal display file.
;
; ============================================================
; Test 3 -- ADR, flag behaviour
; ============================================================
; ADR (16-bit register = register + A) is documented as leaving H/V/Z/C
; unaffected, but a naive implementation lets the internal 8-bit low-byte
; add clobber them anyway (this is what MAME still does today). Calc-
; U-1600's own LH5801 core was changed to preserve flags across ADR -- a
; fix that resolved a real, reproducible Up/Down-key redraw bug -- see
; docs/Up-Down-Key-Investigation.md, "The ADR conflict", before touching
; either the emulator's ADR implementation or this file.
;
; Test: set T = 0x1F (IE|H|V|Z|C all set), set U = 0 and A = 0, execute
; ADR U, then read T back via TTA.
;   Result 0x1F -> flags PRESERVED  (Calc-U-1600's current fix, and the
;                  two reference emulators pc1500emu/forever1500.fr)
;   Result 0x06 -> flags CLOBBERED  (the manual's literal reading; IE
;                  stays set since it isn't one of the ALU flags, but
;                  Z becomes 1 from the 0+0 low-byte add's own result)
; T's value must have IE set (bit 0x02) going in, so the routine never
; leaves maskable interrupts disabled if this hypothesis turns out true
; and returns to BASIC with T genuinely clobbered.
;
; ============================================================
; Layout and calling convention
; ============================================================
; D (0x7C01) is the scratch/result area -- 6 bytes, EQU'd once below so
; it can be relocated. It is NOT the CALL entry point: it holds a live
; data byte (D+0) that DRL/DRR operate on directly, so no code can sit
; there. The routine itself starts right after it, at D+6 (0x7C07 for
; the address below) -- CALL that address from BASIC, not D itself.
;
;   D+0  the byte being rotated -- also the (X) operand for DRL/DRR
;   D+1  A   after DRL
;   D+2  mem after DRL
;   D+3  A   after DRR
;   D+4  mem after DRR
;   D+5  T   after ADR
;
; X is kept pointed at D+0 throughout (it's both the DRL/DRR operand
; pointer and needs no re-pointing between tests); Y walks the D+1..D+5
; result slots. Every store into the result area goes through one of
; these two index registers -- no absolute (ab) addressing is used.
;
; This entry point is called from BASIC via CALL and always ends with
; RTN; it freely clobbers A, X, Y, U and T, the normal PC-1500 ML
; convention.
;
;   PRINT PEEK(&7C02); PEEK(&7C03); PEEK(&7C04); PEEK(&7C05); PEEK(&7C06)
;
; (D+1..D+5 above, once D=0x7C01 -- see the expected-values table at the
; bottom of this file.)
;
; ============================================================
; Assembly command:
;   sdaslh5801 -plosgff instrquirks_1500a.asm
; ============================================================

D           .equ    0x7C01      ; scratch/result area base -- move this
                                 ; one line to relocate the whole test

D_SCRATCH   .equ    D+0
D_A_DRL     .equ    D+1
D_MEM_DRL   .equ    D+2
D_A_DRR     .equ    D+3
D_MEM_DRR   .equ    D+4
D_T_ADR     .equ    D+5

; ============================================================
            .area   CODE (ABS)
            .org    D
; ============================================================

; D+0..D+5 -- scratch/result area. Reserved here, ahead of the code, so
; D_SCRATCH is a genuine, directly (X)-addressable RAM byte and every
; result slot is reachable by walking Y forward one byte at a time.
            .db     0x00        ; D+0  D_SCRATCH
            .db     0x00        ; D+1  D_A_DRL
            .db     0x00        ; D+2  D_MEM_DRL
            .db     0x00        ; D+3  D_A_DRR
            .db     0x00        ; D+4  D_MEM_DRR
            .db     0x00        ; D+5  D_T_ADR

; ============================================================
; START -- entered via CALL from BASIC, e.g. CALL &7C07
; ============================================================
START:
; X = &D_SCRATCH for the whole routine -- both the DRL/DRR operand
; pointer and the source LDA reads back the post-instruction memory
; byte from.
            ldi     xh,>D_SCRATCH
            ldi     xl,<D_SCRATCH

; ------------------------------------------------------------
; Test 1 -- DRL (X)
; ------------------------------------------------------------
            ldi     a,0x34
            sta     (x)         ; D_SCRATCH = 0x34 -- (X) operand primed
            ldi     a,0x12      ; A = 0x12
            drl     (x)         ; D7 -- the instruction under test

; Y walks D_A_DRL, D_MEM_DRL.
            ldi     yh,>D_A_DRL
            ldi     yl,<D_A_DRL
            sta     (y)         ; D_A_DRL = A after DRL
            inc     y
            lda     (x)         ; re-read D_SCRATCH -- DRL's memory result
            sta     (y)         ; D_MEM_DRL = mem after DRL

; ------------------------------------------------------------
; Test 2 -- DRR (X)
; ------------------------------------------------------------
; Re-initialise (X) and A -- DRL above modified D_SCRATCH, so this test
; must not inherit Test 1's output.
            ldi     a,0x34
            sta     (x)         ; D_SCRATCH = 0x34 again
            ldi     a,0x12      ; A = 0x12 again
            drr     (x)         ; D3 -- the instruction under test

; Y walks D_A_DRR, D_MEM_DRR.
            ldi     yh,>D_A_DRR
            ldi     yl,<D_A_DRR
            sta     (y)         ; D_A_DRR = A after DRR
            inc     y
            lda     (x)         ; re-read D_SCRATCH -- DRR's memory result
            sta     (y)         ; D_MEM_DRR = mem after DRR

; ------------------------------------------------------------
; Test 3 -- ADR U, flag behaviour
; ------------------------------------------------------------
            ldi     uh,0x00
            ldi     ul,0x00     ; U = 0x0000

; T can only be written via ATT (T = A & 0x1F), so A must pass through
; 0x1F first. LDI xh/yh/uh/xl/yl/ul never touch flags on this CPU, but
; LDI A does (it sets Z from the loaded value) -- irrelevant here since
; ATT overwrites T outright, byte for byte, regardless of T's incoming
; state.
            ldi     a,0x1F
            att                 ; FD EC -- T = 0x1F (IE|H|V|Z|C all set)

; A must be 0x00 for the ADR operand itself (0+0 low-byte add -> no
; carry, zero result), but "ldi a,0x00" sets Z from the loaded value
; (0x00 -> Z=1) same as the Z bit T already has from the line above, so
; this is a no-op on T -- C/V/H/IE are untouched by any LDI A, per this
; project's LH5801 core (Core/CPU/LH5801/LH5801.cpp, case 0xB5).
            ldi     a,0x00      ; A = 0x00 -- the ADR operand under test
            adr     u           ; FD EA -- the instruction under test:
                                 ; U = U + A (16-bit)
            tta                 ; FD AA -- A = T, read back after ADR
            ldi     yh,>D_T_ADR
            ldi     yl,<D_T_ADR
            sta     (y)         ; D_T_ADR = T after ADR

            rtn                 ; 9A -- back to BASIC

; ============================================================
; Expected PEEK values (D = 0x7C01):
;
;                    (a) MESS/MAME,   (b) 3-nibble    (c) Z80
;                    PockEmul,        rotate           RLD/RRD
;                    forever1500
;                    -- CONFIRMED --     -- WRONG --     -- WRONG --
;   D+1 A/DRL  &7C02   0x34             0x32             0x13
;   D+2 mem/DRL &7C03  0x41             0x41             0x42
;   D+3 A/DRR  &7C04   0x34             0x14             0x14
;   D+4 mem/DRR &7C05  0x23             0x23             0x23
;
;   D+5 T/ADR  &7C06:  0x1F = flags preserved (Calc-U-1600's fix)
;                      0x06 = flags clobbered (manual's literal text)
;
; Confirmed on real PC-1500A hardware: D+1..D+4 read 0x34,0x41,0x34,0x23
; -- reading (a). D+5 not yet independently reported back; ADR's flag-
; preservation fix predates this file and was settled separately (see
; docs/Up-Down-Key-Investigation.md).
; ============================================================
