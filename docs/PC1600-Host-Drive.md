# PC-1600 host-directory drive (S3: / Y:)

**File ▸ Mount Directory…** makes a folder on the computer a PC-1600 drive,
`S3:`. It is also `Y:` when no CE-1600P/CE-1600F is attached. BASIC uses it
like a RAM-disk drive, and the files are ordinary host files.

This is a Calc-U-1600 peripheral with no Sharp original. It has the same
shape as the MEP rev3 module, a 60-pin bus module with a ROM at page-1 bank 7
and a microcontroller behind I/O port 90H. The ROM, the protocol and the host
side are our own.

## What works

| BASIC | Notes |
|---|---|
| `LOAD` / `SAVE` / `BLOAD` / `BSAVE` | Files are stored byte for byte, exactly as the PC-1600 writes them: tokenized BASIC, `SAVE ,A` text, BSAVE headers |
| `OPEN` … `FOR INPUT` / `OUTPUT` / `APPEND`, `INPUT#`, `PRINT#`, `CLOSE` | Set `MAXFILES` first, as for every device |
| `FILES` / `LFILES` | Only 8.3 names, see below |
| `KILL`, `NAME`, `COPY`, `SET "…","P"` / `"I"` / `" "` | Wildcards work as on the RAM disk: every matching file, protected ones skipped (error 159) |
| `DSKF("S3:")` | The host volume's free space, capped at 2 147 450 880 bytes, the most DSKF can compute |
| `INIT "S3:"` | Refused: a format must never wipe a host folder |

Nothing is converted on the way. A text file written on the computer needs
CR LF line ends to read line by line with `INPUT#`.

## Which host files are visible

The PC-1600 sees regular files whose name fits 8.3 and uses only the
characters its file-name parser accepts (FNCHAROK, PC1600-P1-B3B 79E2H):
letters, digits and ``` `@#$%^&()-_{}' ```.

- Names are shown upper-case: `prog.bas` is `PROG.BAS`.
- Long names, extra dots, spaces, non-ASCII names, directories and dotfiles
  are not shown and are never touched.
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
| `OUT (91H)` | function (FFH = reset: drop searches; nothing follows) |
| `OUT (90H)` × n | DE lo, DE hi, DEVNAME (FC16H), FCB+00H..+38H; for 15H also the 256 bytes at (DMA) |
| `IN (90H)` × n | status, ERL, FCB+00H..+38H, payload length lo/hi, payload → (DMA), BC, DE, HL (lo/hi) |

- The host runs the call when the last request byte arrives.
- `IN (91H)` reads 00H (always ready).
- SET DMA (1AH) stays in the ROM.
- The protocol deliberately differs from the MEP's, so that all file logic
  lives in testable C++.

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
