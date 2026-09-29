; ============================================================================
; Calc-U-1600 host-directory drive -- PC-1600 ROM module, Page 1 / Bank 7
; (4000H-7FFFH). Original work, part of Calc-U-1600.
;
; Build: tools/build_hostdrive_rom.sh (zasm) -> PC1600-P1-B7-HOSTDRIVE.bin,
; committed next to this file. docs/PC1600-Host-Drive.md has the design.
;
; OVERVIEW
; A ROM module (ID 43H,16H) that makes a directory on the host computer a
; PC-1600 file device, "S3:" (device code 42H), alias "Y:" (43H). The ROM
; only forwards: every FILE IOCS call (01DEH, C = 0FH..23H, DE = FCB) goes
; over I/O ports 90H/91H to the emulator's PC1600HostDriveCard, which does
; the work on the host file system (HostDirectoryDrive) and answers with the
; IOCS status, ERL, the updated FCB and any data for the DMA buffer.
;
; SCANMODS (07C5H) finds the module at reset; FILE_I (105FH) matches the
; device table and jumps to the file handler (+15H) with C = function, DE =
; FCB, page 1 = this bank. A CE-1600F (bank 5) is searched first, so with a
; CE-1600P attached "Y:" is its second floppy drive and only "S3:" comes
; here. The token table has no INIT: INIT "S3:" finds no handler and is
; refused.
;
; PROTOCOL (one transaction per FILE call)
;   OUT (91H),fn        start: function byte (FFH = module reset: OUT (90H)
;                       the +02H function code, nothing comes back)
;   OUT (90H) x ...     DE lo, DE hi, DEVNAME, FCB+00H..+38H (57 bytes),
;                       and for 15H (write) the 256 bytes at (DMAADR)
;   IN  (90H) x ...     status, ERL, FCB+00H..+38H (57 bytes),
;                       length lo, length hi, <length> bytes -> (DMAADR),
;                       BC lo/hi, DE lo/hi, HL lo/hi
;
; MEP COMPATIBILITY
; Subdirectories work as on the MEP rev3 module: the host keeps a current
; directory that every FILE call works in. Programs written for the MEP
; (e.g. FILEX) call its fixed entries 4020H/4023H/4026H in bank 7 directly,
; and BASIC gets the MEP's statements CDIR "path" and LDIR under the MEP's
; tokens F2D0H / F2D1H:
;   OUT (91H),FCH       CDIR: OUT (90H) length, path bytes; IN (90H)
;                       status, ERL, and on success the 27-byte prompt
;                       ("S3:/DEV/ASM>", CR) -> MEPPROMPT (FB10H)
;   OUT (91H),FDH       FILEMODE: SEARCH FIRST/NEXT list files (default)
;   OUT (91H),FEH       DIRMODE: SEARCH FIRST/NEXT list subdirectories
; ============================================================================

P_HDDATA:   equ 90H     ; host drive: request / response byte stream
P_HDCMD:    equ 91H     ; host drive: write = start a transaction (function)

HDRESETCMD: equ 0FFH    ; transaction without payload: drop open searches
HDDIRCMD:   equ 0FEH    ; transaction without payload: DIRMODE
HDFILECMD:  equ 0FDH    ; transaction without payload: FILEMODE
HDCDIRCMD:  equ 0FCH    ; CDIR: length and path follow, status and ERL back
FCBHDRLEN:  equ 39H     ; FCB+00H..+38H travel; the buffer at +39H does not

DEVNAME:    equ 0FC16H  ; device code of the FILE call (set by FILE_I)
DMAADR:     equ 0FC46H  ; FILE 1AH SET DMA address
ERL:        equ 0F89BH  ; error code of the last file operation
PROCPTR:    equ 0FE00H  ; interpreter processing pointer
PORT3D_M:   equ 0F07DH  ; mirror of the write-only port 3DH
XX_TYPE:    equ 0FA04H  ; XX+4: >= B3H for a string
XX_STRAD:   equ 0FA05H  ; XX+5/+6: string address (high without b7, low)
XX_STRLEN:  equ 0FA07H  ; XX+7: string length
MEPPROMPT:  equ 0FB10H  ; CDIR prompt, the MEP's documented result buffer
PROMPTLEN:  equ 27      ; 26 characters + CR

P_BANK:     equ 31H     ; bank select (b3-b1 = page 1)
P_ROMSEL:   equ 3DH     ; bank 3 variant: 00H = bank 3b

PRTASTR:    equ 00EBH   ; print the text at DE up to the character A
CRSRSET:    equ 0115H   ; cursor to column D, row E
CRSRPOS:    equ 0118H   ; cursor position -> D column, E row
UPSCRL:     equ 012DH   ; scroll the display up one row
EOCHK2:     equ 025CH   ; Z = statement end at (HL)
EXPRESS:    equ 0274H   ; evaluate an expression at (HL) -> XX
COMMADR:    equ 02BCH   ; token DE -> handler HL, bank field C

TOKFILES:   equ 0F098H  ; built-in FILES statement (bank 3b)

	org 4000H



;------------------------------------------------------------------------------
; 4000H - ROM module header (same layout as banks 3 and 5)
;------------------------------------------------------------------------------
MODID:
	defb 43H,16H
	jp HDRESET              ; +02H: reset / power functions (A = function)
	ret                     ; +05H: interrupt hook: nothing
	nop
	nop
	scf                     ; +08H: device IOCS 80H..8FH: none
	ret
	nop
	scf                     ; +0BH: spare
	ret
	nop
	jp HDNOAUTO             ; +0EH: AUTORUN.BAS search: not supported
	defw HDDEVTAB           ; +11H: device-name table
	defw HDTOKTAB           ; +13H: token table (CDIR, LDIR)
	jp HDFILE               ; +15H: file handler
	defs 4020H-$,0C9H       ; +18H..+1FH: RET, as the MEP module

	jp HDCDIR               ; 4020H: MEP CDIR (DE = path, B = length)
	jp HDDIRMODE            ; 4023H: MEP DIRMODE
	jp HDFILEMODE           ; 4026H: MEP FILEMODE

HDDEVTAB:
	defb "S3",00H,00H,42H
	defb "Y",00H,00H,00H,43H
	defb 00H

	defb "Calc-U-1600 host drive 1.1",00H



;------------------------------------------------------------------------------
; Token table: the letter index (27 words, "C" and "L" pointing at the
; length byte of their entry), then the entries (bank field 0EH = page-1
; bank 7, handler, token, attribute, length, name) as TOKFIND (11D6H)
; walks them from table+39H. Tokens and attributes are the MEP's, so a
; program with CDIR / LDIR lines runs on both.
;------------------------------------------------------------------------------
HDTOKTAB:
	defw 0000H,0000H,0000H  ; index: -, A, B
	defw TOKCDIR+6          ; C
	defw 0000H,0000H,0000H,0000H,0000H,0000H,0000H,0000H ; D..K
	defw TOKLDIR+6          ; L
	defw 0000H,0000H,0000H,0000H,0000H,0000H,0000H ; M..S
	defw 0000H,0000H,0000H,0000H,0000H,0000H,0000H ; T..Z
TOKCDIR:
	defb 0EH
	defw CMDCDIR
	defw 0F2D0H
	defb 0BDH,4,"CDIR"
TOKLDIR:
	defb 0EH
	defw CMDLDIR
	defw 0F2D1H
	defb 0ADH,4,"LDIR"
	defb 00H,00H,00H,00H,00H,00H,00H



;------------------------------------------------------------------------------
; Reset / power functions (module +02H)
; Passes the function code to the host, which forgets searches in progress
; on every call and goes back to the root on power on and reset (a MEP
; loses its current directory with the power). Returns like the other
; modules: A = 0..2 -> A := 0, NC; everything else CY.
;------------------------------------------------------------------------------
HDRESET:
	push af
	ld a,HDRESETCMD
	out (P_HDCMD),a
	pop af
	out (P_HDDATA),a
	cp 03H
	jr nc,HDRESETCY
	xor a
	ret
HDRESETCY:
	scf
	ret



;------------------------------------------------------------------------------
; AUTORUN search (module +0EH): A := 9BH (device not available)
;------------------------------------------------------------------------------
HDNOAUTO:
	ld a,9BH
	ret



;------------------------------------------------------------------------------
; MEP fixed entry 4020H: change the directory
;
;   Entry parameters: DE = path ("/" root, ".", "..", relative), B = length
; Modified registers: AF, BC, HL; MEPPROMPT = "S3:/path>" + CR on success
;   Error conditions: CY, A = IOCS status, ERL set (no such directory:
;                     01H, 98H file not found)
;------------------------------------------------------------------------------
HDCDIR:
	ld a,HDCDIRCMD
	out (P_HDCMD),a
	ld c,P_HDDATA
	out (c),b
	ex de,hl                ; HL = path
	ld a,b
	or a
	jr z,HDCDIRST
	otir
HDCDIRST:
	ex de,hl
	in a,(c)                ; IOCS status
	in b,(c)
	ld hl,ERL
	ld (hl),b
	or a
	jr nz,HDCDIRERR
	ld hl,MEPPROMPT
	ld b,PROMPTLEN
	inir
	ret                     ; A = 0, NC
HDCDIRERR:
	scf
	ret



;------------------------------------------------------------------------------
; BASIC statement CDIR "path": change the directory, show the prompt on the
; next display row
;
;   Entry parameters: HL = processing pointer after the token
; Modified registers: HL = processing pointer (PROCPTR); all others
;   Error conditions: CY and A = BASIC error code (01H syntax, 07H not a
;                     string, 12H empty or 255+ characters, ERL of CDIR)
;------------------------------------------------------------------------------
CMDCDIR:
	call EXPRESS
	jr c,CMDERR01
	ld a,(XX_TYPE)
	cp 0B3H
	jr c,CMDERR07
	ld hl,(XX_STRAD)        ; L = high (b7 clear), H = low
	ld a,h
	ld h,l
	ld l,a
	set 7,h
	ld a,(XX_STRLEN)
	or a
	jr z,CMDERR12
	cp 0FFH
	jr nc,CMDERR12
	ld b,a
	ex de,hl                ; DE = path
	push de
	push bc
	ld hl,(PROCPTR)
	call EOCHK2
	pop bc
	pop de
	jr nz,CMDERR01
	call HDCDIR
	jr nc,CMDCDIROK
	ld a,(ERL)
	scf
	ret
CMDCDIROK:
	call CRSRPOS            ; to the start of the next row
	ld d,00H
	ld a,e
	cp 03H
	jr c,CMDNEXTROW
	call UPSCRL
	ld e,02H
CMDNEXTROW:
	inc e
	call CRSRSET
	ld de,MEPPROMPT
	ld a,0DH
	call PRTASTR
	ld hl,(PROCPTR)         ; the statement continues from here
	xor a
	ret

CMDERR01:
	ld a,01H
	scf
	ret
CMDERR07:
	ld a,07H
	scf
	ret
CMDERR12:
	ld a,12H
	scf
	ret



;------------------------------------------------------------------------------
; BASIC statement LDIR: list the subdirectories -- the built-in FILES "S3:"
; in directory mode
; FILES lives in bank 3b of page 1, where this ROM is too, so it runs from a
; trampoline copied onto the stack: it maps bank 3b, calls FILES with the
; text "S3:" as its processing pointer, and maps this bank back.
;
;   Entry parameters: HL = processing pointer after the token
; Modified registers: HL = processing pointer (PROCPTR); all others
;   Error conditions: CY and A = BASIC error code (12H: parameters given;
;                     those of FILES)
;------------------------------------------------------------------------------
CMDLDIR:
	call EOCHK2
	jr nz,CMDERR12
	ld de,TOKFILES
	call COMMADR            ; HL = handler, C = its bank field
	ld a,c
	and 0EH
	ld c,a
	in a,(P_BANK)
	and 0F1H
	or c
	ld b,a                  ; B = port 31H for FILES
	ex de,hl                ; DE = handler
	ld hl,-TRAMPLEN
	add hl,sp
	ld sp,hl                ; HL = SP = trampoline
	push hl
	push de
	push bc
	ex de,hl
	ld hl,LDIRTRAMP
	ld bc,TRAMPLEN
	ldir
	pop bc
	pop de
	pop ix                  ; IX = trampoline
	ld (ix+TRBANK),b
	ld (ix+TRCALL),e
	ld (ix+TRCALL+1),d
	in a,(P_BANK)
	ld (ix+TROLDBANK),a
	ld a,(PORT3D_M)
	ld (ix+TROLD3D),a
	push ix
	pop hl
	ld de,TRTEXT
	add hl,de               ; HL = the text "S3:"
	ld (ix+TRTEXTP),l
	ld (ix+TRTEXTP+1),h
	ld de,(PROCPTR)
	push de
	ld (PROCPTR),hl
	ld a,HDDIRCMD
	out (P_HDCMD),a         ; DIRMODE
	ld hl,CMDLDIRRET
	push hl
	jp (ix)
CMDLDIRRET:
	pop hl
	ld (PROCPTR),hl
	push af
	pop de                  ; DE = AF of FILES
	ld hl,TRAMPLEN
	add hl,sp
	ld sp,hl                ; drop the trampoline
	push de
	ld a,HDFILECMD
	out (P_HDCMD),a         ; FILEMODE again
	ld hl,(PROCPTR)         ; the statement continues from here
	pop af
	ret

; The trampoline, copied onto the stack; TR... are offsets of the bytes
; CMDLDIR fills in.
LDIRTRAMP:
	xor a                   ; port 3DH := 00H: bank 3b
	out (P_ROMSEL),a
	ld (PORT3D_M),a
TRBANKI:
	ld a,00H                ; port 31H with page 1 = FILES's bank
	out (P_BANK),a
TRTEXTI:
	ld hl,0000H             ; HL = processing pointer = the text
	xor a
TRCALLI:
	call 0000H              ; FILES
	push af
TROLD3DI:
	ld a,00H                ; the caller's port 3DH
	out (P_ROMSEL),a
	ld (PORT3D_M),a
TROLDBANKI:
	ld a,00H                ; the caller's port 31H: this bank again
	out (P_BANK),a
	pop af
	ret
LDIRTEXT:
	defb 22H,"S3:",22H,0DH
TRAMPLEN:   equ $-LDIRTRAMP
TRBANK:     equ TRBANKI+1-LDIRTRAMP     ; operand offsets
TRTEXTP:    equ TRTEXTI+1-LDIRTRAMP
TRCALL:     equ TRCALLI+1-LDIRTRAMP
TROLD3D:    equ TROLD3DI+1-LDIRTRAMP
TROLDBANK:  equ TROLDBANKI+1-LDIRTRAMP
TRTEXT:     equ LDIRTEXT-LDIRTRAMP



;------------------------------------------------------------------------------
; MEP fixed entries 4023H / 4026H: what SEARCH FIRST / NEXT list, until the
; other one or a module reset. Return A = 0, NC.
;------------------------------------------------------------------------------
HDDIRMODE:
	ld a,HDDIRCMD
	jr HDMODE
HDFILEMODE:
	ld a,HDFILECMD
HDMODE:
	out (P_HDCMD),a
	xor a
	ret



;------------------------------------------------------------------------------
; File handler (module +15H), entered from FILE (01DEH)
;
;   Entry parameters: C = IOCS function, DE = FCB, (DMAADR) = buffer
; Modified registers: BC, DE, HL as returned by the host (GET ALLOC: BC =
;                     bytes/sector, E = sectors/cluster, HL = free clusters;
;                     GET LENGTH: DE:HL = records); AF
;   Error conditions: A = IOCS status (0 = OK), ERL set
;------------------------------------------------------------------------------
HDFILE:
	ld a,c
	cp 1AH                  ; SET DMA stays in the PC-1600
	jr z,HDSETDMA
	push de
	out (P_HDCMD),a         ; start the transaction
	ld c,P_HDDATA
	out (c),e
	out (c),d
	ld hl,DEVNAME
	outi
	ex de,hl                ; HL = FCB
	ld b,FCBHDRLEN
	otir
	cp 15H                  ; SEQUENTIAL WRITE: the record follows
	jr nz,HDREPLY
	ld hl,(DMAADR)
	ld b,00H                ; 256 bytes
	otir
HDREPLY:
	pop hl                  ; HL = FCB
	in a,(c)                ; IOCS status
	push af
	in a,(c)
	ld (ERL),a
	ld b,FCBHDRLEN
	inir                    ; FCB+00H..+38H back
	in b,(c)                ; payload length lo (0 with hi 1 = 256)
	in a,(c)                ; payload length hi
	or b
	jr z,HDREGS
	ld hl,(DMAADR)
	inir
HDREGS:
	in e,(c)
	in d,(c)
	push de                 ; BC
	in e,(c)
	in d,(c)                ; DE
	in l,(c)
	in h,(c)                ; HL
	pop bc
	pop af
	or a
	ret

HDSETDMA:
	ld (DMAADR),de
	xor a
	ld (ERL),a
	ret

	defs 8000H-$,0FFH       ; rest of the bank: FFH, no module at 6000H
