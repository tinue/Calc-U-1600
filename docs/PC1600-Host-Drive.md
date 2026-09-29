# PC-1600 host-directory drive (S3: / Y:)

**File ▸ Mount Directory…** makes a folder on the computer a PC-1600 drive,
`S3:`. It is also `Y:` when no CE-1600P/CE-1600F is attached. BASIC uses it
like a RAM-disk drive, and the files are ordinary host files.

This is a Calc-U-1600 peripheral with no Sharp original. It has the same
shape as the MEP rev3 module, a 60-pin bus module with a ROM at page-1 bank 7
and a microcontroller behind I/O port 90H. The ROM, the protocol and the host
side are our own. To software it looks like a MEP rev3 USB drive: the same
device names, subdirectories through `CDIR` / `LDIR`, and the MEP's fixed
entries, so MEP programs such as FILEX ([PC1600-FILEX.md](PC1600-FILEX.md))
work on it.

## What works

| BASIC | Notes |
|---|---|
| `LOAD` / `SAVE` / `BLOAD` / `BSAVE` | Files are stored byte for byte, exactly as the PC-1600 writes them: tokenized BASIC, `SAVE ,A` text, BSAVE headers |
| `OPEN` … `FOR INPUT` / `OUTPUT` / `APPEND`, `INPUT#`, `PRINT#`, `CLOSE` | Set `MAXFILES` first, as for every device |
| `FILES` / `LFILES` | Only 8.3 names, see below |
| `KILL`, `NAME`, `COPY`, `SET "…","P"` / `"I"` / `" "` | Wildcards work as on the RAM disk: every matching file, protected ones skipped (error 159) |
| `DSKF("S3:")` | The host volume's free space, capped at 2 147 450 880 bytes, the most DSKF can compute |
| `INIT "S3:"` | Refused: a format must never wipe a host folder |
| `CDIR "path"` | Changes the current directory and shows the prompt, e.g. `S3:/DEV/ASM>`. `/` starts at the top folder, `..` goes up, `.` stays |
| `LDIR` | Lists the subdirectories of the current directory, in the FILES format |

Nothing is converted on the way. A text file written on the computer needs
CR LF line ends to read line by line with `INPUT#`.

## Which host files are visible

The PC-1600 sees regular files whose name fits 8.3 and uses only the
characters its file-name parser accepts (FNCHAROK, PC1600-P1-B3B 79E2H):
letters, digits and ``` `@#$%^&()-_{}' ```.

- Names are shown upper-case: `prog.bas` is `PROG.BAS`.
- Long names, extra dots, spaces, non-ASCII names and dotfiles are not shown
  and are never touched.
- Subdirectories follow the same name rule. FILES lists only the files of the
  current directory; `LDIR` lists its subdirectories. Symbolic links to
  folders are left out, so the drive never leaves the mounted folder.
- On a case-sensitive file system, two files that fold to the same name show
  as one. The exact upper-case spelling wins, otherwise the first in sort
  order.
- New files are created upper-case. Writing to an existing file keeps its
  host spelling.

## Attributes and dates

- **P** (protected) is the host file's owner-write permission. SET "P"
  clears it, and SET " " sets it again.
- **I** (invisible) is the macOS hidden flag (`chflags hidden`). On other
  systems it is never set.
- **Directory dates** are the host modification time in local time, with the
  **year fixed at 1986**. That is year field 6, which the ROM's own FATTIME
  writes because the PC-1600 clock has no year. FILES shows only mm/dd
  hh:mm.
- **On the host side** the file keeps its real time stamp. The OS writes it
  on every change, from the computer's clock.

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
    and no token table.
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

### S3 compared with a real MEP rev3

| Area | MEP rev3 | S3 |
|---|---|---|
| Subdirectories, CDIR / LDIR, fixed entries 4020H/4023H/4026H, prompt at FB10H | Yes | Yes |
| Current directory after power-on | Top folder | Top folder |
| APPEND, DSKF, SET, GET LENGTH | ERROR 158 | Supported |
| Files open at once | One for reading, one for writing | As many as MAXFILES |
| Names | 8.3 recommended | Only 8.3 names are visible |
| INIT | Not handled | Refused |

### FILE functions

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

## GUI

- **Mount Directory…** (PC-1600 only) plugs the drive in with the same OFF/ON
  cycle as a plotter attach, because the ROM only finds modules at power-on.
- **Mount again** while mounted swaps the folder live.
- **Unmount** power-cycles the drive out.
- The mount survives a ROM switch but not a model switch, and is not saved
  across launches.
- **Preset:** `host-drive: <dir>` mounts a folder before the preset's cold
  boot. The path is relative to the preset, or starts with `~/`. The GUI then
  shows the drive under File ▸ Unmount (`examples/dwx/DiskWorks.pc1600`).

## Tests

- `Core/tests/host_directory_drive_tests.cpp`: the host side on temporary
  folders.
- `Core/tests/pc1600_host_drive_tests.cpp`: end to end with the real ROM and
  BASIC typed at the prompt.
