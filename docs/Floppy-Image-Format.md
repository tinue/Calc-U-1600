# CE-1600F Floppy Image Format (`*.floppy.yaml`)

The file format Calc-U-1600 uses to store a CE-1600F diskette. This document is the
specification; the reference reader/writer is `Core/Connector/FloppyImageFile.hpp`.
Other tools that read or write these files (e.g. SharpDataExchange's `sde dir/get/put/del`)
implement this document, not the C++ source.

## 1. Scope

The container holds the raw bytes of both sides of one 2.5″ diskette — nothing else.
It knows nothing about files: the PC-1600's FAT-style filesystem inside a side is Sharp's
format, documented in `SharpPC1500Reference/PC-1600/PC-1600-Filesystem.md` §5 and
`PC-1600-Peripherals-Hardware.md` §2. The emulator itself never interprets it either; the
CE-1600P ROM does, as on real hardware.

## 2. File naming and lookup

- File name: anything ending in `.floppy.yaml`.
- A disk is identified by its `disk-name`, not its file name. A preset's `floppy:` key and
  the GUI picker refer to the disk name. Lookup searches the bundled directory first, then
  the user's save folder; a name resolves to the first directory holding exactly one file
  that declares it.
- A disk declaring `template: true` is a **template**: the app never writes to it,
  wherever it is stored (every bundled disk is one; a user may drop their own into the
  save folder, and it is then listed with the bundled ones). Name & Save copies a template
  into a new file without the key. A disk without it is an **instance**: the app autosaves
  it in place.

## 3. Syntax

A small YAML subset (the one `Core/Yaml.hpp` reads):

- block mappings by indentation (spaces only — **tabs are rejected**);
- `#` whole-line comments and ` #` trailing comments outside quotes;
- scalars bare, `"double-quoted"` or `'single-quoted'` — **no escape processing**;
- `key: |` literal block scalars (common indent stripped).

Keys may appear in any order. Unknown keys are an error at every level.

## 4. Keys

| Key | Required | Value |
|---|---|---|
| `format` | yes | `ce1600f-floppy` |
| `format-version` | yes | integer; this document is version **1** |
| `disk-name` | yes | non-empty string. Writers double-quote it; it therefore must not contain `"` or a newline |
| `template` | no | `true` = read-only template (see §2); absent or `false` = instance. Never written by the app |
| `saved` | no | ISO-8601 UTC timestamp `YYYY-MM-DDTHH:MM:SSZ` of the last write (informational) |
| `sides` | yes | mapping with exactly the keys `a` and `b` |
| `sides.a`, `sides.b` | yes | mapping with exactly `encoding` and `bytes` |
| `…encoding` | yes | `addressed-hex` |
| `…bytes` | yes | `|` block scalar in `addressed-hex` covering exactly `0x10000` bytes |

**Versioning.** A reader must reject any `format-version` it does not know rather than
guess. Any change a version-1 reader would misread requires a new version number.

## 5. Sides and byte order

- Each side is **65536 bytes**: 16 tracks × 8 sectors × 512 bytes. The drive is
  single-sided and the medium is flipped by hand, so each side is its own volume.
- Byte offset within a side = **logical sector × 512**, where logical sector =
  track × 8 + sector (sectors numbered from 0). This is the numbering the CE-1600P ROM's
  IOCS uses, so the filesystem's boot sector is at `$0000`, the FAT at `$0200`, its copy at
  `$0400`, the directory at `$0600`–`$0BFF` and the first data cluster at `$0C00`.
- Sector ID fields, gaps and other on-track formatting are not stored.

## 6. `addressed-hex`

As defined in `Memory-Card-Definition-Format.md` §6:

- `$XXXX: <1–16 space-separated hex byte pairs>` — literal bytes from that address;
- `$XXXX: XX...` (the only token on its line) — byte `XX` repeated up to the next line's
  address, or to the end of the side for the last line;
- lines, in address order, must partition `[0, 0x10000)` exactly: no gap, no overlap.

### Canonical writer form

A reader accepts any valid `addressed-hex`, but writers **should** emit the canonical form
so that files diff cleanly whichever tool saved them:

- the first line is `# Calc-U-1600 CE-1600F floppy disk image`, then `format`,
  `format-version`, `disk-name`, `saved`, `sides` in that order;
- 2-space indent per level (`  a:`, `    encoding:`, `      $0000: …`), `\n` line endings;
- rows of 16 bytes, uppercase hex, `$` plus a 4-digit uppercase address, one space between
  bytes and **two** before the 9th;
- a row whose 16 bytes are identical starts a run `$XXXX: XX...`, which absorbs every
  following whole row of the same byte;
- `saved` is set to the current UTC time on every write;
- the file is always rewritten whole.

## 7. Example

```yaml
# Calc-U-1600 CE-1600F floppy disk image
format: ce1600f-floppy
format-version: 1
disk-name: "Formatted"
template: true
saved: 2026-09-19T10:17:46Z
sides:
  a:
    encoding: addressed-hex
    bytes: |
      $0000: 00...
      $0200: F2 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00
      $0210: 00...
      $0400: F2 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00
      $0410: 00...
  b:
    encoding: addressed-hex
    bytes: |
      $0000: 00...
```

(Side B here is unformatted; side A has just been through `INIT`.)

## 8. Editing an image with another tool

Calc-U-1600 keeps an inserted disk in memory and rewrites the whole file about half a
second after every write the PC-1600 makes to it, so a change made by another tool while
the disk is inserted is lost at the next such write. **Eject the disk (or quit the
emulator) before editing its file**, and re-insert it afterwards.
