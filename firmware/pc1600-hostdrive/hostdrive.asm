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
; here. No token table: INIT "S3:" finds no handler and is refused.
;
; PROTOCOL (one transaction per FILE call)
;   OUT (91H),fn        start: function byte (FFH = reset, no further bytes)
;   OUT (90H) x ...     DE lo, DE hi, DEVNAME, FCB+00H..+38H (57 bytes),
;                       and for 15H (write) the 256 bytes at (DMAADR)
;   IN  (90H) x ...     status, ERL, FCB+00H..+38H (57 bytes),
;                       length lo, length hi, <length> bytes -> (DMAADR),
;                       BC lo/hi, DE lo/hi, HL lo/hi
;
; MEP FIXED ENTRIES
; Programs written for the MEP rev3 module (e.g. FILEX) take "S3:" for the
; MEP and call its fixed entries 4020H/4023H/4026H in bank 7 directly. The
; same addresses here give them a drive without subdirectories:
;   OUT (91H),FCH       CDIR: OUT (90H) length, path bytes;
;                       IN (90H) status, ERL (only the root "/" exists)
;   OUT (91H),FDH       FILEMODE: SEARCH FIRST/NEXT list files (default)
;   OUT (91H),FEH       DIRMODE: SEARCH FIRST/NEXT list directories (none)
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
	defw 0000H              ; +13H: no token table
	jp HDFILE               ; +15H: file handler
	defs 4020H-$,0C9H       ; +18H..+1FH: RET, as the MEP module

	jp HDCDIR               ; 4020H: MEP CDIR (DE = path, B = length)
	jp HDDIRMODE            ; 4023H: MEP DIRMODE
	jp HDFILEMODE           ; 4026H: MEP FILEMODE

HDDEVTAB:
	defb "S3",00H,00H,42H
	defb "Y",00H,00H,00H,43H
	defb 00H

	defb "Calc-U-1600 host drive 1.0",00H



;------------------------------------------------------------------------------
; Reset / power functions (module +02H)
; Tells the host to forget searches in progress on every call (new, power
; on/off, reset, boot). Returns like the other modules: A = 0..2 -> A := 0,
; NC; everything else CY.
;------------------------------------------------------------------------------
HDRESET:
	push af
	ld a,HDRESETCMD
	out (P_HDCMD),a
	pop af
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
;   Entry parameters: DE = path, B = length
; Modified registers: AF, BC, HL
;   Error conditions: CY, A = IOCS status, ERL set (a path other than the
;                     root: 01H, 98H file not found)
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
	ret z
	scf
	ret



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
