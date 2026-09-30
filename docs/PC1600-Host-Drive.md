# PC-1600 host-directory drive (S3: / Y:)

**File ▸ Mount Directory…** makes a folder on the computer a PC-1600 drive,
`S3:`. It is also `Y:` when no CE-1600P/CE-1600F is attached. BASIC uses it
like a RAM-disk drive, and the files are ordinary host files.

This is a Calc-U-1600 peripheral with no Sharp original. It is modelled on
the MEP rev3 module, a USB drive for the PC-1600's expansion bus. To
software it looks like a MEP rev3 USB drive: the same
device names, subdirectories through `CDIR` / `LDIR`, and the MEP's fixed
entries, so MEP programs such as FILEX ([PC1600-FILEX.md](PC1600-FILEX.md))
work on it. The MEP is credited in
[THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md#acknowledgments).

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
characters its file-name parser accepts:
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

## Mounting a folder

- **Mount Directory…** (PC-1600 only) plugs the drive in with the same OFF/ON
  cycle as a plotter attach, because the ROM only finds modules at power-on.
  The dialog opens in **Settings ▸ Host drive folder**: the directory last
  mounted by default, or a fixed folder.
- **Mount again** while mounted swaps the folder live.
- **Unmount** power-cycles the drive out.
- The mount survives a ROM switch but not a model switch, and is not saved
  across launches.
- **Preset:** `host-drive: <dir>` mounts a folder before the preset's cold
  boot. The path is relative to the preset, or starts with `~/`. The GUI then
  shows the drive under File ▸ Unmount (`examples/dwx/DiskWorks.pc1600`).

## Compared with a real MEP rev3

| Area | MEP rev3 | S3 |
|---|---|---|
| Subdirectories, CDIR / LDIR, the MEP's fixed ROM entries | Yes | Yes |
| Current directory after power-on | Top folder | Top folder |
| APPEND, DSKF, SET, GET LENGTH | ERROR 158 | Supported |
| Files open at once | One for reading, one for writing | As many as MAXFILES |
| Names | 8.3 recommended | Only 8.3 names are visible |
| INIT | Not handled | Refused |

How the drive is built (driver ROM, I/O protocol, FILE functions) is in
[developer/PC1600-Host-Drive-Internals.md](developer/PC1600-Host-Drive-Internals.md).
