# PC-1600 host-directory drive — internals

How the `S3:` / `Y:` host-directory drive is built. What it does for the
user is in [../PC1600-Host-Drive.md](../PC1600-Host-Drive.md); the reasons
behind its oddities are in [../background/Decisions.md](../background/Decisions.md).

It has the same shape as the MEP rev3 module: a 60-pin bus module with a ROM
at page-1 bank 7 and a microcontroller behind I/O port 90H. The ROM, the
protocol and the host side are our own. The visible-name rule follows the
ROM's file-name parser (FNCHAROK, PC1600-P1-B3B 79E2H).

## Hardware model

```
PC-1600 BASIC ──FILE 01DEH──▶ FILE_I (105FH) ──device "S3"/"Y"──▶ driver ROM (bank 7, 4000H)
                                                                    │ OUT 91H / OUT 90H … / IN 90H …
                                                                    ▼
                                             PC1600HostDriveCard ──▶ HostDirectoryDrive ──▶ host folder
```

- **Driver ROM** `firmware/pc1600-hostdrive/hostdrive.asm`, assembled by
  `tools/build_hostdrive_rom.sh` (zasm). The `.bin` and `.lst` are committed
  and bundled with the app.
  - It is a ROM module (ID `43 16`) with device table `S3`=42H and `Y`=43H,
    and a token table at +13H (CDIR, LDIR; see BASIC below).
  - 4020H/4023H/4026H are the MEP's fixed entries CDIR, DIRMODE and
    FILEMODE (see below), so the device table starts at 4029H.
  - SCANMODS (07C5H) finds it at power-on/reset.
  - FILE_I searches modules in bank order, so a CE-1600F in bank 5 keeps
    `Y:`.
- **`Core/Connector/PC1600HostDriveCard.hpp`** sits on the 60-pin system bus.
  It serves the ROM when page B selects bank 7, and I/O ports 90H/91H.
- **`Core/Connector/HostDirectoryDrive.hpp`** holds the FILE functions on the
  host file system. They follow the ROM's own file devices: the CE-1600F
  (FDFILEHND 4267H, PC1600-P1-B5) and the RAM disk (B3FILEHND 49B2H,
  PC1600-P1-B3).

### Protocol

One transaction per FILE call:

| Direction | Bytes |
|---|---|
| `OUT (91H)` | function (FFH = module reset: `OUT (90H)` the +02H function code, nothing comes back; the host drops searches and returns to file mode, and on power-on/resume/reset to the top folder) |
| `OUT (90H)` × n | DE lo, DE hi, DEVNAME (FC16H), FCB+00H..+38H; for 15H also the 256 bytes at (DMA) |
| `IN (90H)` × n | status, ERL, FCB+00H..+38H, payload length lo/hi, payload → (DMA), BC, DE, HL (lo/hi) |

- The host runs the call when the last request byte arrives.
- `IN (91H)` reads 00H (always ready).
- SET DMA (1AH) stays in the ROM.
- The protocol deliberately differs from the MEP's, so that all file logic
  lives in testable C++.

### Subdirectories (MEP compatible)

The drive keeps a current directory, and every FILE call works in it. A
file name never has a path, just as on the MEP. The public side matches the
MEP rev3 manual and ROM, so its software runs unchanged:

| Entry | MEP name | ROM → host | Behaviour |
|---|---|---|---|
| 4020H | CDIR (DE = path, B = length) | `OUT (91H)` FCH, length, path; `IN` status, ERL, then 27 prompt bytes on success | UNIX-style path: `/` absolute, `..` up (the top folder stays the top), `.` stays; components match visible 8.3 folders in any case. The prompt `S3:/DEV/ASM>` + CR goes to FB10H, the MEP's buffer. A prompt longer than 26 characters keeps the end (`S3:../ASM>`). An unknown folder gives CY, 01H, ERL 98H and leaves the directory unchanged |
| 4023H | DIRMODE | `OUT (91H)` FEH | SEARCH FIRST/NEXT list the subdirectories, sorted, without `.`/`..` |
| 4026H | FILEMODE | `OUT (91H)` FDH | SEARCH FIRST/NEXT list files again (the default) |

- **Directory-mode entries** carry attribute 00H, size 0 and the folder's
  modification time. They can't carry FAT's 10H: FILES skips every entry
  with a bit of DCH set (PC1600-P1-B3B K_FILES 6AD5H), and `LDIR` is FILES in
  directory mode.
- **BASIC:** the token table (module +13H) holds `CDIR` = F2D0H and `LDIR` =
  F2D1H, the MEP's tokens and attributes, so tokenized programs run on both.
  `CDIR` takes a string (07H if not, 12H if empty); `LDIR` takes nothing
  (12H).
- **LDIR** runs the built-in FILES (token F098H, bank 3b) on the text
  `"S3:"`. FILES is in page 1 like this ROM, so a small trampoline copied onto
  the stack maps bank 3b, calls it and maps bank 7 back. The MEP copies its
  trampoline to LISTBUF (FBB0H). We don't, because a direct command is
  tokenized there.
- **The current directory** goes back to the top folder on power-on, on the
  power-on after an APO and on reset: a MEP loses power with the calculator.
  NEW and boot keep it. A new mount starts at the top. If the current folder
  is deleted on the computer, file calls fail with 01H/98H until the next
  `CDIR`, so nothing lands in another folder.
- 4018H..401FH are RET, as on the MEP.

## FILE functions

| C | Function | Behaviour |
|---|---|---|
| 0FH | OPEN | Directory entry +0BH..+1FH → FCB+14H..+28H, record length 256 at +2EH, record 0 |
| 10H | CLOSE | For a written file (FCB+36H bits 0/1), trims the last record to FCB+06H bytes, as FDCLOSE does |
| 11H/12H | SEARCH FIRST/NEXT | 32-byte entry → (DMA). FCB+2DH is FFH at the end. The list is taken at SEARCH FIRST |
| 13H | DELETE | All matches; protected ones skipped (9FH) |
| 14H | READ | Record FCB+30H (7 bits) + FCB+31H/32H × 128. Past the end: status 04H, ERL A2H |
| 15H | WRITE | A whole 256-byte record; the file ends after it |
| 16H | CREATE | Truncates or creates; a protected file gives 9FH |
| 17H | RENAME | New name at FCB+29H; '?' keeps the old character. An existing target gives 97H |
| 1BH | GET ALLOC | BC = 512, E = 64, HL = free clusters (≤ FFFFH) |
| 1EH | SET ATTRB | Bits 01H/02H as above |
| 23H | GET LENGTH | DE:HL = records of FCB+2EH bytes |
| others | – | 10H / ERL 9EH |

No directory mounted: status 20H, ERL A0H.

## Tests

- `Core/tests/host_directory_drive_tests.cpp`: the host side on temporary
  folders.
- `Core/tests/pc1600_host_drive_tests.cpp`: end to end with the real ROM and
  BASIC typed at the prompt.
