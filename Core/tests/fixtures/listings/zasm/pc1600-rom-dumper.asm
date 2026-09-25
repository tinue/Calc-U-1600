; ============================================================================
; PC-1600 ROM Bank Dumper
;
; Menu-driven Z-80 ML program for the Sharp PC-1600:
;
;   1 = SWEEP -- pages every known Z-80-addressable ROM bank into the CPU's
;       address space in turn and shows a short hex sample on the LCD,
;       pausing for a keypress between screens. Diagnostic tool: confirms
;       which banks hold real ROM vs. open bus, and lets you eyeball a
;       bank's content before committing to a full dump.
;
;   2 = DUMP+SEND -- pages in the 7 banks confirmed to hold real ROM content
;       (see dump_table below), shows a 16-bit checksum per page, and on a
;       keypress sends the full 16KB page out over COM1: via the serial
;       IOCS (CSNDA). Press S to skip a page instead of sending it. COM1:
;       itself must already be configured (SETCOM/OUTSTAT/SNDSTAT/RCVSTAT/
;       INIT) from BASIC before this program is CALLed -- see the
;       accompanying .pc1600 preset / README for the exact sequence. This
;       program only calls CSNDA; it never touches CWCOM/CESND/CCLRSB.
;
;   3 = LH5803 ROM -- dumps the LH-5803 co-processor's own private 16KB ROM
;       (LH-5803 view C000H-FFFFH), which no Z-80 bank-switching combination
;       can reach -- it's the *other* half of the same physical chip as
;       Bank 6/CS123, addressed only from the LH-5803's own side. Reached
;       via the CALLH bridge (CALL 01C6H): a 2-byte LH-5801 stub (`lda (x)`
;       / `rtn`) placed in the shared 4000-7FFF/C000-FFFF RAM fetches one
;       byte per round-trip, using X as the source pointer and A/PARA as
;       the return path. Same checksum-then-keypress-then-send UX as
;       DUMP+SEND, but as a single fixed 16KB region rather than 7 banks.
;
; Runs entirely from C000-FFFF (page 3, bank 0 -- never touched by this
; program) so paging banks into 4000-7FFF/8000-BFFF never disturbs the code
; that is doing the paging. All display/IOCS calls used here live in the
; always-resident bank 0 (0000-3FFF), so they too are unaffected.
;
; Assumes the machine is already NEW'd with enough S0: space to hold this
; program; the .pc1600 preset / load sequence handles that.
;
; Hardware note -- Port 3DH (hidden-BASIC-ROM sub-bank select, bit b2):
; clearing b2 selects Bank 3b instead of Bank 3 at 4000-7FFF. This port is
; write-only. Getting a correct read of Bank 3/3b requires two things this
; program does everywhere it touches the port:
;   1. BANKSET before the Port 3DH write, not after (the ROM's own BANKSET
;      routine does not preserve an out-of-order Port 3DH state).
;   2. Interrupts briefly disabled (DI/EI) around the write and the read
;      that follows it. The PC-1600's own interrupt handlers (keyboard
;      scan / the 1/64s timer tick) do not preserve Port 3DH's state, so a
;      read landing between the write and the actual memory access can
;      silently see the wrong bank. For the serial send specifically (too
;      long, ~17s at 9600 baud, to run under one DI), Port 3DH is instead
;      re-asserted before every byte, each reassert individually DI/EI-
;      guarded.
; ============================================================================

#target bin

; ---- IOCS entry points (TRM, all in Bank 0 / always resident) ----
BANKSET     equ 0x0190      ; A=bank, B=page(0-3) -> set bank
BANKREAD    equ 0x0193      ; B=page -> A=current bank
MEMORYCHK   equ 0x018D      ; D=bank, E=addr high byte -> CF=1/A=00 none, CF=0/A=01 RAM, CF=0/A=03 ROM
CLS         equ 0x0112
CRSRSET     equ 0x0115      ; D=X(0-25), E=Y(0-3)
PRTASTR     equ 0x00EB      ; DE=addr, A=terminator byte -> prints until terminator
KEYGET      equ 0x0166      ; waits for a key; CF=0 -> A=key code

; ---- Serial IOCS gateway (PC-1600-Serial-Commands.md Part 2) ----
SERIAL      equ 0x01D8      ; C=routine#, D=channel(1=COM1:) as needed -> dispatch
S_CSNDA     equ 0x03        ; A=byte to send -> CF=1/A=error byte on failure

PORT3D      equ 0x3D        ; hidden-BASIC-ROM sub-bank select (bit 2; not readable via IN)

; ---- Z-80 -> LH-5803 bridge (PC-1600-CPU-LH5803-Compat.md Part 3) ----
CALLH       equ 0x01C6      ; hands off to an LH-5803 subroutine, returns when it RTNs
CMDZ        equ 0xF002      ; 20H = no params in, 30H = load PARA..PARUH before entry
PARA        equ 0xF005      ; -> LH-5803 A on entry; <- LH-5803 A on return
PARXL       equ 0xF006      ; -> LH-5803 XL (low byte of the X reg pair)
PARXH       equ 0xF007      ; -> LH-5803 XH (high byte)
PARPCL      equ 0xF00C      ; subroutine entry address, low byte (LH-5803-view)
PARPCH      equ 0xF00D      ; subroutine entry address, high byte
PARBAN      equ 0xF00E      ; subroutine bank: 00H = PV(0), 01H = PV(1)

#code CODE, 0xC0C5

; ============================================================================
; Entry point: menu -- 1 = sweep, 2 = dump+send, 3 = LH5803 ROM.
start:
        call CLS
        ld   hl, lbl_menu1
        call show_label
        ld   d, 0
        ld   e, 1
        call CRSRSET
        ld   hl, lbl_menu2
        ld   d, h
        ld   e, l
        xor  a
        call PRTASTR

        ld   d, 0
        ld   e, 2
        call CRSRSET
        ld   hl, lbl_menu3
        ld   d, h
        ld   e, l
        xor  a
        call PRTASTR

menu_wait:
        call KEYGET
        cp   '1'
        jp   z, sweep_start
        cp   '2'
        jp   z, dump_start
        cp   '3'
        jp   z, lh5803_dump_start
        jr   menu_wait

; ============================================================================
; Sweep: walk every known bank, page 0/1/2, showing a hex sample on the LCD.
sweep_start:
        call CLS

        ; ---- Page 0 sample: bank 0 is always resident, no BANKSET needed ----
        ld   hl, lbl_page0
        call show_label
        ld   a, 0                  ; "bank 0" for the BR=/CHK= line (informational only)
        ld   d, 0
        ld   e, 0x00
        call show_status           ; MEMORYCHK D=0,E=00 -- page-0 bank 0
        ld   a, 0x04                ; page 0 doesn't involve Port 3DH
        ld   (cur_port3d), a
        ld   hl, 0x0000
        ld   de, line_buf
        call BUILD_HEX_LINE
        ld   d, 0
        ld   e, 2
        ld   hl, line_buf
        call DISPLAY_LINE_AT
        call wait_key

        ; ---- Page 1 sweep ----
        ld   ix, page1_table
p1_loop:
        ld   a, (ix+0)
        cp   0xFF
        jp   z, p2_start
        call run_page1_entry
        push ix
        pop  hl
        ld   de, 5      ; page1_table record size: bank,port3d,mode,lbl_lo,lbl_hi
        add  hl, de
        push hl
        pop  ix
        jr   p1_loop

; ============================================================================
p2_start:
        ; ---- Page 2 sweep ----
        ld   ix, page2_table
p2_loop:
        ld   a, (ix+0)
        cp   0xFF
        jp   z, all_done
        call run_page2_entry
        push ix
        pop  hl
        ld   de, 4      ; page2_table record size: bank,mode,lbl_lo,lbl_hi
        add  hl, de
        push hl
        pop  ix
        jr   p2_loop

; ============================================================================
all_done:
        ; Restore known-good defaults before returning to BASIC:
        ; page 1 -> bank 0, page 2 -> bank 6 (system ROM), hidden-BASIC off.
        xor  a
        ld   b, 1
        call BANKSET
        ld   a, 6
        ld   b, 2
        call BANKSET
        ld   a, 0x04
        out  (PORT3D), a

        call CLS
        ld   hl, lbl_done
        call show_label
        ret

; ============================================================================
; Dump+send: page in each of the 7 confirmed-ROM banks in turn, show a
; checksum, wait for a keypress (S = skip, anything else = send), then send
; the full 16KB page over COM1:.
dump_start:
        call CLS

        ld   ix, dump_table
dump_loop:
        ld   a, (ix+0)
        cp   0xFF
        jp   z, dump_done
        call run_dump_entry
        jr   nc, dump_next
        ld   (send_err_code), a     ; CSNDA reported an error -- save the error
        jp   dump_error             ; byte before CLS/show_label clobber A
dump_next:
        push ix
        pop  hl
        ld   de, 9      ; dump_table record size: bank,port3d,page,base_hi,
                         ; base_lo,lbl_lo,lbl_hi,fn_lo,fn_hi
        add  hl, de
        push hl
        pop  ix
        jr   dump_loop

; ============================================================================
dump_error:
        call CLS
        ld   hl, lbl_error
        call show_label
        ld   hl, status_buf
        ld   (hl), 'E' \ inc hl
        ld   (hl), 'R' \ inc hl
        ld   (hl), '=' \ inc hl
        ld   a, (send_err_code)
        call hex_byte
        ld   (hl), 0
        ld   d, 0
        ld   e, 1
        call CRSRSET
        ld   hl, status_buf
        ld   d, h
        ld   e, l
        xor  a
        call PRTASTR
        ret

; ============================================================================
dump_done:
        ; Restore known-good defaults before returning to BASIC, same as
        ; Stage 1's all_done:.
        xor  a
        ld   b, 1
        call BANKSET
        ld   a, 6
        ld   b, 2
        call BANKSET
        ld   a, 0x04
        out  (PORT3D), a

        call CLS
        ld   hl, lbl_done
        call show_label
        ret

; ============================================================================
; run_dump_entry: (ix) = { bank, port3d, page, base_hi, base_lo, lbl_lo,
;                           lbl_hi, fn_lo, fn_hi }
;   port3d = 0xFF means "don't touch" (page 0/page 2 entries).
;   page = 0 means "always resident, no BANKSET call" (the page-0 entry).
; Returns: CF/A as SEND_PAGE (CF=1, A=error byte, on a CSNDA failure).
run_dump_entry:
        ld   a, (ix+2)              ; page number
        or   a
        jr   z, rde_no_bankset      ; page 0 -- always resident, nothing to switch
        ld   c, a                   ; c = page number
        ld   a, (ix+0)              ; a = bank number
        ld   b, c                   ; b = page number
        call BANKSET
rde_no_bankset:
        ld   a, (ix+1)              ; port3d value, or 0xFF = don't touch
        cp   0xFF
        jr   nz, rde_have_port3d
        ld   a, 0x04                ; "don't touch" -> the safe/normal default
rde_have_port3d:
        ld   (cur_port3d), a        ; SEND_PAGE reasserts this every byte later

        di
        out  (PORT3D), a

        ; checksum over the 16KB page, base address = (ix+3):(ix+4) --
        ; the actual hardware read, done immediately, interrupts disabled.
        ld   l, (ix+4)
        ld   h, (ix+3)
        call COMPUTE_CHECKSUM       ; HL preserved, DE = checksum
        ei
        push de                     ; stash across the display calls below

        call CLS
        ld   l, (ix+5)
        ld   h, (ix+6)
        call show_label

        ; filename hint on line 1
        ld   d, 0
        ld   e, 1
        call CRSRSET
        ld   l, (ix+7)
        ld   h, (ix+8)
        ld   d, h
        ld   e, l
        xor  a
        call PRTASTR

        pop  de                     ; recover the checksum computed above
        ld   hl, status_buf
        ld   (hl), 'C' \ inc hl
        ld   (hl), 'H' \ inc hl
        ld   (hl), 'K' \ inc hl
        ld   (hl), '=' \ inc hl
        ld   a, d
        call hex_byte
        ld   a, e
        call hex_byte
        ld   (hl), 0
        ld   d, 0
        ld   e, 2
        call CRSRSET
        ld   hl, status_buf
        ld   d, h
        ld   e, l
        xor  a
        call PRTASTR

        ld   d, 0
        ld   e, 3
        call CRSRSET
        ld   hl, lbl_press_key
        ld   d, h
        ld   e, l
        xor  a
        call PRTASTR
        call KEYGET

        cp   'S'                      ; skip this page -- no re-send of an
        jr   z, rde_skip              ; already-captured page (no display
        cp   's'                      ; hint needed; any other key sends)
        jr   z, rde_skip

        ; Re-assert bank/Port 3DH immediately before the actual send: the
        ; checksum above was read under DI, but the display/keypress wait
        ; since then ran with interrupts enabled and may have disturbed
        ; Port 3DH again. SEND_PAGE itself reasserts before every byte too
        ; (the send is far too long to cover with a single DI), so this is
        ; just what gets the very first bytes right before its first
        ; reassert point.
        ld   a, (ix+2)              ; page number
        or   a
        jr   z, rde_no_bankset2
        ld   c, a
        ld   a, (ix+0)              ; bank number
        ld   b, c
        call BANKSET
rde_no_bankset2:
        ld   a, (ix+1)              ; port3d value, or 0xFF = don't touch
        cp   0xFF
        jr   z, rde_no_port3d2
        di
        out  (PORT3D), a
        ei
rde_no_port3d2:

        ld   l, (ix+4)               ; reload base address -- clobbered above
        ld   h, (ix+3)
        jp   SEND_PAGE                ; tail call: CF/A propagate to caller

rde_skip:
        xor  a                       ; success, nothing sent, CF=0
        ret

; ============================================================================
; COMPUTE_CHECKSUM: HL = base address of a 16KB (0x4000-byte) page.
; Returns DE = 16-bit additive checksum (sum of all bytes, mod 65536).
; HL is preserved.
COMPUTE_CHECKSUM:
        push hl
        ld   bc, 0x4000
        ld   de, 0x0000
cs_loop:
        ld   a, (hl)
        add  a, e
        ld   e, a
        jr   nc, cs_noc
        inc  d
cs_noc:
        inc  hl
        dec  bc
        ld   a, b
        or   c
        jr   nz, cs_loop
        pop  hl
        ret

; ============================================================================
; SEND_PAGE: HL = base address of a 16KB (0x4000-byte) page. Sends every
; byte over COM1: via CSNDA. On success returns CF=0. On a CSNDA error,
; stops immediately and returns CF=1, A=CSNDA's error byte (b0=timeout,
; b1=BREAK pressed) -- no channel parameter needed, per the IOCS table.
;
; Port 3DH is re-asserted from cur_port3d before every byte (each reassert
; individually DI/EI-guarded; CSNDA itself runs with interrupts enabled,
; since the send is too long -- ~17s at 9600 baud -- to cover with a
; single DI without starving whatever the serial IOCS needs interrupts
; for).
SEND_PAGE:
        ld   bc, 0x4000
sp_loop:
        di
        ld   a, (cur_port3d)
        out  (PORT3D), a
        ei
        ld   a, (hl)
        push hl
        push bc
        ld   c, S_CSNDA
        call SERIAL
        pop  bc
        pop  hl
        jr   c, sp_error
        inc  hl
        dec  bc
        ld   a, b
        or   c
        jr   nz, sp_loop
        xor  a                      ; success: A=0, CF=0
        ret
sp_error:
        scf                          ; CF=1; A already holds CSNDA's error byte
        ret

; ============================================================================
; LH5803_FETCH_BYTE: HL = LH-5803-view source address to read. Returns the
; fetched byte in A. Runs the 2-byte `lda (x) / rtn` stub (lh5803_stub) on
; the LH-5803 via CALLH, with X preloaded from HL (CMDZ=30H) and A read back
; from PARA on return.
;
; CALLH clobbers all Z-80 registers (documented) -- callers must not expect
; anything but A to survive a call here, and must keep their own loop state
; (address/counter/accumulator) in memory, not registers, across it.
LH5803_FETCH_BYTE:
        ld   a, 0x30                ; CMDZ=30H: load PARA..PARUH before entry
        ld   (CMDZ), a
        ld   (PARXL), hl            ; HL -> PARXL/PARXH (L=low, H=high)
        ld   hl, lh5803_stub - 0x8000  ; stub's LH-5803-view entry address
        ld   (PARPCL), hl
        xor  a
        ld   (PARBAN), a            ; PV(0) -- neither the stub's home (4000-7FFF)
                                     ; nor the ROM being read (C000-FFFF) is
                                     ; documented as PV-banked
        di
        call CALLH
        ei
        ld   a, (PARA)
        ret

; ============================================================================
; LH5803_SELFTEST: fetches LH-5803 address C000H twice in a row and compares
; the two results. Returns CF=0 if they agree, CF=1 if they don't. Mirrors
; the double-fetch check that originally caught the Port 3DH interrupt bug --
; run once before trusting a full LH5803 dump.
LH5803_SELFTEST:
        ld   hl, 0xC000
        call LH5803_FETCH_BYTE
        ld   (lh5803_selftest_tmp), a
        ld   hl, 0xC000
        call LH5803_FETCH_BYTE
        ld   b, a
        ld   a, (lh5803_selftest_tmp)
        cp   b
        jr   z, lh5803_selftest_ok
        scf
        ret
lh5803_selftest_ok:
        or   a                      ; clear carry
        ret

; ============================================================================
; LH5803_COMPUTE_CHECKSUM: sums all 16384 bytes of LH-5803 C000H-FFFFH via
; LH5803_FETCH_BYTE. Returns DE = 16-bit additive checksum (mod 65536). All
; loop state lives in memory (lh5803_addr/lh5803_count/lh5803_chk), not
; registers, since LH5803_FETCH_BYTE's CALLH clobbers everything.
LH5803_COMPUTE_CHECKSUM:
        ld   hl, 0xC000
        ld   (lh5803_addr), hl
        ld   hl, 0x4000
        ld   (lh5803_count), hl
        ld   hl, 0x0000
        ld   (lh5803_chk), hl
lcs_loop:
        ld   hl, (lh5803_addr)
        call LH5803_FETCH_BYTE      ; A = byte
        ld   hl, (lh5803_chk)
        add  a, l
        ld   l, a
        jr   nc, lcs_noc
        inc  h
lcs_noc:
        ld   (lh5803_chk), hl
        ld   hl, (lh5803_addr)
        inc  hl
        ld   (lh5803_addr), hl
        ld   hl, (lh5803_count)
        dec  hl
        ld   (lh5803_count), hl
        ld   a, h
        or   l
        jr   nz, lcs_loop
        ld   de, (lh5803_chk)
        ret

; ============================================================================
; LH5803_SEND: sends all 16384 bytes of LH-5803 C000H-FFFFH over COM1: via
; CSNDA, fetching each one through LH5803_FETCH_BYTE. Same CF/A error-return
; convention as SEND_PAGE. No Port 3DH involvement at all -- this path never
; touches Z-80 bank switching.
LH5803_SEND:
        ld   hl, 0xC000
        ld   (lh5803_addr), hl
        ld   hl, 0x4000
        ld   (lh5803_count), hl
lsend_loop:
        ld   hl, (lh5803_addr)
        call LH5803_FETCH_BYTE      ; A = byte
        ld   c, S_CSNDA
        call SERIAL
        jr   c, lsend_error
        ld   hl, (lh5803_addr)
        inc  hl
        ld   (lh5803_addr), hl
        ld   hl, (lh5803_count)
        dec  hl
        ld   (lh5803_count), hl
        ld   a, h
        or   l
        jr   nz, lsend_loop
        xor  a                      ; success: A=0, CF=0
        ret
lsend_error:
        scf                          ; CF=1; A already holds CSNDA's error byte
        ret

; ============================================================================
; lh5803_dump_start: menu option 3. Self-test, then checksum (with a
; "COMPUTING..." message -- 16384 CALLH round-trips may take a visible
; moment), then the usual checksum/press-key/send UX.
lh5803_dump_start:
        call CLS
        ld   hl, lbl_lh5803
        call show_label
        ld   d, 0
        ld   e, 1
        call CRSRSET
        ld   hl, lbl_computing
        ld   d, h
        ld   e, l
        xor  a
        call PRTASTR

        call LH5803_SELFTEST
        jr   c, lh5803_unstable

        call LH5803_COMPUTE_CHECKSUM   ; DE = checksum
        push de                        ; stash across the display calls below

        call CLS
        ld   hl, lbl_lh5803
        call show_label

        ld   d, 0
        ld   e, 1
        call CRSRSET
        ld   hl, fn_lh5803
        ld   d, h
        ld   e, l
        xor  a
        call PRTASTR

        pop  de                        ; recover the checksum computed above
        ld   hl, status_buf
        ld   (hl), 'C' \ inc hl
        ld   (hl), 'H' \ inc hl
        ld   (hl), 'K' \ inc hl
        ld   (hl), '=' \ inc hl
        ld   a, d
        call hex_byte
        ld   a, e
        call hex_byte
        ld   (hl), 0
        ld   d, 0
        ld   e, 2
        call CRSRSET
        ld   hl, status_buf
        ld   d, h
        ld   e, l
        xor  a
        call PRTASTR

        ld   d, 0
        ld   e, 3
        call CRSRSET
        ld   hl, lbl_press_key
        ld   d, h
        ld   e, l
        xor  a
        call PRTASTR
        call KEYGET

        cp   'S'
        jr   z, lh5803_done
        cp   's'
        jr   z, lh5803_done

        call LH5803_SEND
        jr   nc, lh5803_done
        ld   (send_err_code), a        ; CSNDA reported an error -- save the
        jp   dump_error                ; error byte before CLS/show_label clobber A

lh5803_done:
        ret

lh5803_unstable:
        call CLS
        ld   hl, lbl_lh5803_unstable
        call show_label
        ret

; ============================================================================
; run_page1_entry: (ix) = { bank, port3d, mode, lo(label), hi(label) }
;   mode: 0 = normal sample at 4000H, 1 = bank-5 style, samples at 5000H/6000H
run_page1_entry:
        ld   a, (ix+0)             ; bank number
        ld   b, 1                  ; page 1 = 4000-7FFF
        call BANKSET

        ld   a, (ix+1)             ; port3d value
        ld   (cur_port3d), a
        di
        out  (PORT3D), a

        ld   a, (ix+2)             ; mode
        cp   1
        jr   z, p1_bank5_read

        ld   hl, 0x4000
        ld   de, line_buf
        call BUILD_HEX_LINE
        jr   p1_reads_done

p1_bank5_read:
        ld   hl, 0x5000
        ld   de, line_buf
        call BUILD_HEX_LINE
        ld   hl, 0x6000
        ld   de, line_buf2
        call BUILD_HEX_LINE

p1_reads_done:
        ei

p1_display:
        call CLS
        ld   l, (ix+3)
        ld   h, (ix+4)
        call show_label

        ld   a, (ix+0)
        ld   d, a
        ld   e, 0x40
        call show_status

        ld   a, (ix+2)             ; mode
        cp   1
        jr   z, p1_bank5_show

        ld   d, 0
        ld   e, 2
        ld   hl, line_buf
        call DISPLAY_LINE_AT
        jr   p1_entry_done

p1_bank5_show:
        ld   d, 0
        ld   e, 2
        ld   hl, line_buf
        call DISPLAY_LINE_AT
        ld   d, 0
        ld   e, 3
        ld   hl, line_buf2
        call DISPLAY_LINE_AT

p1_entry_done:
        jp   wait_key

; ============================================================================
; run_page2_entry: (ix) = { bank, mode, lo(label), hi(label) }
;   mode: 0 = MEMORYCHK only, 1 = full hex sample too
run_page2_entry:
        ld   a, (ix+0)
        ld   b, 2                  ; page 2 = 8000-BFFF
        call BANKSET

        call CLS
        ld   l, (ix+2)
        ld   h, (ix+3)
        call show_label

        ld   a, (ix+0)
        ld   d, a
        ld   e, 0x80
        call show_status

        ld   a, (ix+1)
        cp   1
        jr   nz, p2_entry_done

        ld   a, 0x04                ; page 2 doesn't involve Port 3DH
        ld   (cur_port3d), a
        ld   hl, 0x8000
        ld   de, line_buf
        call BUILD_HEX_LINE
        ld   d, 0
        ld   e, 2
        ld   hl, line_buf
        call DISPLAY_LINE_AT

p2_entry_done:
        jp   wait_key

; ============================================================================
; show_label: HL = zero-terminated string; shown on line Y=0
show_label:
        push hl
        ld   d, 0
        ld   e, 0
        call CRSRSET
        pop  hl
        ld   d, h
        ld   e, l
        xor  a
        call PRTASTR
        ret

; ============================================================================
; show_status: D=bank, E=addr-high-byte for MEMORYCHK; builds/shows the
; "BR=xx CHK=X(xx)" line on Y=1. BR = BANKREAD confirmation for whichever
; page was just set (page number inferred from E: 0x40->page1, 0x80->page2,
; 0x00->page0); CHK = MEMORYCHK result (tag letter + raw hex byte). Note:
; MEMORYCHK does not report bank presence reliably on this firmware
; (confirmed on real hardware) -- treat its output as informational only;
; the hex-sample inspection is the reliable signal for bank content.
show_status:
        push de
        ld   a, e
        cp   0x80
        jr   z, ss_p2
        cp   0x40
        jr   z, ss_p1
        ld   b, 0                  ; page 0
        jr   ss_bankread
ss_p1:  ld   b, 1
        jr   ss_bankread
ss_p2:  ld   b, 2
ss_bankread:
        call BANKREAD              ; A = current bank for page B

        ld   hl, status_buf
        ld   (hl), 'B' \ inc hl
        ld   (hl), 'R' \ inc hl
        ld   (hl), '=' \ inc hl
        call hex_byte
        ld   (hl), ' ' \ inc hl
        ld   (hl), 'C' \ inc hl
        ld   (hl), 'H' \ inc hl
        ld   (hl), 'K' \ inc hl
        ld   (hl), '=' \ inc hl

        pop  de                    ; D=bank, E=addr-high, restored for MEMORYCHK
        push hl
        call MEMORYCHK             ; CF/A set per bank presence
        jr   c, ss_none
        cp   1
        jr   z, ss_ram
        cp   3
        jr   z, ss_rom
        ld   c, '?'
        jr   ss_tag
ss_none: ld  c, 'N'
        jr   ss_tag
ss_ram: ld   c, 'R'
        jr   ss_tag
ss_rom: ld   c, 'M'
ss_tag:
        pop  hl
        ld   (hl), c \ inc hl
        ld   (hl), '(' \ inc hl
        call hex_byte
        ld   (hl), ')' \ inc hl
        ld   (hl), 0

        ld   d, 0
        ld   e, 1
        call CRSRSET
        ld   hl, status_buf
        ld   d, h
        ld   e, l
        xor  a
        call PRTASTR
        ret

; ============================================================================
; BUILD_HEX_LINE: HL = base address to sample 8 bytes from, DE = dest
; buffer. Fills DE with "AAAA:xxxxxxxxxxxxxxxx" (null-terminated). Pure
; computation -- no IOCS calls -- so it can run immediately after a Port
; 3DH/BANKSET write with nothing else in between.
BUILD_HEX_LINE:
        push hl
        ld   a, h
        call hex_byte_to_de
        ld   a, l
        call hex_byte_to_de
        ld   a, ':'
        ld   (de), a
        inc  de
        pop  hl

        ld   b, 8
bhl_loop:
        ld   a, (hl)
        push hl
        push bc
        ex   de, hl
        call hex_byte
        ex   de, hl
        pop  bc
        pop  hl
        inc  hl
        djnz bhl_loop

        xor  a
        ld   (de), a
        ret

; ============================================================================
; DISPLAY_LINE_AT: D=x, E=y (set by caller), HL = buffer to display
; (already built by BUILD_HEX_LINE). Just CRSRSET + PRTASTR.
DISPLAY_LINE_AT:
        push hl
        call CRSRSET
        pop  hl
        ld   d, h
        ld   e, l
        xor  a
        call PRTASTR
        ret

; hex_byte_to_de: A=byte, DE=dest ptr (advanced by 2) -- same as hex_byte but
; writes through DE instead of HL, for the address prefix above.
hex_byte_to_de:
        push hl
        ex   de, hl
        call hex_byte
        ex   de, hl
        pop  hl
        ret

; ============================================================================
; hex_byte: A=byte to convert, HL=dest ptr; writes 2 ASCII hex chars, HL+=2
hex_byte:
        push af
        rrca
        rrca
        rrca
        rrca
        call hex_nibble
        pop  af
hex_nibble:
        and  0x0F
        cp   10
        jr   c, hn1
        add  a, 7
hn1:
        add  a, '0'
        ld   (hl), a
        inc  hl
        ret

; ============================================================================
wait_key:
        call KEYGET
        ret

; ============================================================================
; Page-1 sweep table: { bank, port3d, mode, label_lo, label_hi }, 0xFF ends it
page1_table:
        db 0, 0x04, 0, lo(lbl_p1_b0), hi(lbl_p1_b0)
        db 1, 0x04, 0, lo(lbl_p1_b1), hi(lbl_p1_b1)
        db 2, 0x04, 0, lo(lbl_p1_b2), hi(lbl_p1_b2)
        db 3, 0x04, 0, lo(lbl_p1_b3), hi(lbl_p1_b3)
        db 3, 0x00, 0, lo(lbl_p1_b3b), hi(lbl_p1_b3b)
        db 4, 0x04, 0, lo(lbl_p1_b4), hi(lbl_p1_b4)
        db 5, 0x04, 1, lo(lbl_p1_b5), hi(lbl_p1_b5)
        db 6, 0x04, 0, lo(lbl_p1_b6), hi(lbl_p1_b6)
        db 7, 0x04, 0, lo(lbl_p1_b7), hi(lbl_p1_b7)
        db 0xFF

; Page-2 sweep table: { bank, mode, label_lo, label_hi }, 0xFF ends it
page2_table:
        db 0, 0, lo(lbl_p2_b0), hi(lbl_p2_b0)
        db 1, 0, lo(lbl_p2_b1), hi(lbl_p2_b1)
        db 2, 0, lo(lbl_p2_b2), hi(lbl_p2_b2)
        db 3, 0, lo(lbl_p2_b3), hi(lbl_p2_b3)
        db 4, 0, lo(lbl_p2_b4), hi(lbl_p2_b4)
        db 6, 1, lo(lbl_p2_b6), hi(lbl_p2_b6)
        db 7, 0, lo(lbl_p2_b7), hi(lbl_p2_b7)
        db 0xFF

; ============================================================================
; Dump table: { bank, port3d, page, base_hi, base_lo, lbl_lo, lbl_hi, fn_lo,
; fn_hi }, 0xFF ends it. port3d=0xFF means don't touch; page=0 means no
; BANKSET (page-0 bank-0 is always resident). Covers the 7 banks confirmed
; to hold real ROM content: bank 0 (both page-0 and page-1 windows -- one
; physical chip), bank 3 + bank 3b (CS24's two halves), bank 4 + bank 5
; (CE-1600P peripheral ROM), and page-2 bank 6 (CS123's Z-80-visible half).
dump_table:
        db 0, 0xFF, 0, 0x00,0x00, lo(lbl_d_p0b0),hi(lbl_d_p0b0),   lo(fn_p0b0),hi(fn_p0b0)
        db 0, 0x04, 1, 0x40,0x00, lo(lbl_d_p1b0),hi(lbl_d_p1b0),   lo(fn_p1b0),hi(fn_p1b0)
        db 3, 0x04, 1, 0x40,0x00, lo(lbl_d_p1b3),hi(lbl_d_p1b3),   lo(fn_p1b3),hi(fn_p1b3)
        db 3, 0x00, 1, 0x40,0x00, lo(lbl_d_p1b3b),hi(lbl_d_p1b3b), lo(fn_p1b3b),hi(fn_p1b3b)
        db 4, 0x04, 1, 0x40,0x00, lo(lbl_d_p1b4),hi(lbl_d_p1b4),   lo(fn_p1b4),hi(fn_p1b4)
        db 5, 0x04, 1, 0x40,0x00, lo(lbl_d_p1b5),hi(lbl_d_p1b5),   lo(fn_p1b5),hi(fn_p1b5)
        db 6, 0xFF, 2, 0x80,0x00, lo(lbl_d_p2b6),hi(lbl_d_p2b6),   lo(fn_p2b6),hi(fn_p2b6)
        db 0xFF

; ============================================================================
lbl_menu1:      db "PC1600 ROM DUMPER", 0
lbl_menu2:      db "1=SWEEP  2=DUMP+SEND", 0
lbl_menu3:      db "3=LH5803 ROM", 0
lbl_error:      db "SEND ERROR - ABORTED", 0
lbl_press_key:  db "PRESS KEY TO SEND", 0

lbl_lh5803:            db "LH5803 ROM C000-FFFF", 0
lbl_computing:          db "COMPUTING...", 0
lbl_lh5803_unstable:    db "LH5803 FETCH UNSTABLE", 0
fn_lh5803:              db "PC1600-LH5803-C000-FFFF.BIN", 0

lbl_d_p0b0:  db "DUMP P0 0000H BANK0", 0
lbl_d_p1b0:  db "DUMP P1 4000H BANK0", 0
lbl_d_p1b3:  db "DUMP P1 4000H BANK3", 0
lbl_d_p1b3b: db "DUMP P1 4000H BANK3B", 0
lbl_d_p1b4:  db "DUMP P1 4000H BANK4", 0
lbl_d_p1b5:  db "DUMP P1 4000H BANK5", 0
lbl_d_p2b6:  db "DUMP P2 8000H BANK6", 0

fn_p0b0:     db "PC1600-P0-B0.BIN", 0
fn_p1b0:     db "PC1600-P1-B0.BIN", 0
fn_p1b3:     db "PC1600-P1-B3.BIN", 0
fn_p1b3b:    db "PC1600-P1-B3B.BIN", 0
fn_p1b4:     db "PC1600-P1-B4-CE1600P.BIN", 0
fn_p1b5:     db "PC1600-P1-B5-CE1600P-OR-F.BIN", 0
fn_p2b6:     db "PC1600-P2-B6.BIN", 0

; ============================================================================
lbl_page0:   db "PAGE0 (0000H) BANK0", 0
lbl_done:    db "DONE - BASIC RESTORED", 0

lbl_p1_b0:   db "P1 4000H BANK0", 0
lbl_p1_b1:   db "P1 4000H BANK1", 0
lbl_p1_b2:   db "P1 4000H BANK2", 0
lbl_p1_b3:   db "P1 4000H BANK3", 0
lbl_p1_b3b:  db "P1 4000H BANK3B", 0
lbl_p1_b4:   db "P1 4000H BANK4 CE1600P", 0
lbl_p1_b5:   db "P1 BANK5 CE1600P FD/CAS", 0
lbl_p1_b6:   db "P1 4000H BANK6", 0
lbl_p1_b7:   db "P1 4000H BANK7", 0

lbl_p2_b0:   db "P2 8000H BANK0", 0
lbl_p2_b1:   db "P2 8000H BANK1", 0
lbl_p2_b2:   db "P2 8000H BANK2", 0
lbl_p2_b3:   db "P2 8000H BANK3", 0
lbl_p2_b4:   db "P2 8000H BANK4 KANJI", 0
lbl_p2_b6:   db "P2 8000H BANK6 CS123", 0
lbl_p2_b7:   db "P2 8000H BANK7", 0

; ============================================================================
; lh5803_stub: the LH-5801 machine code run on the LH-5803 via CALLH.
;   lda (x)  -- opcode 0x05 -- A = byte at the address in X (ME0)
;   rtn      -- opcode 0x9A -- return to the Z-80 side
; X is preloaded from PARXL/PARXH (CMDZ=30H) before this runs, so that's
; the whole program. Lives here, inside the same C000-FFFF RAM this program
; already owns while running -- its LH-5803-view address is computed at
; assemble time in LH5803_FETCH_BYTE as (lh5803_stub - 0x8000).
lh5803_stub: db 0x05, 0x9A

; ---- scratch buffers ----
status_buf:             ds 24
line_buf:               ds 24
line_buf2:              ds 24
send_err_code:          ds 1
cur_port3d:             ds 1
lh5803_addr:            ds 2
lh5803_count:           ds 2
lh5803_chk:             ds 2
lh5803_selftest_tmp:    ds 1

#end
