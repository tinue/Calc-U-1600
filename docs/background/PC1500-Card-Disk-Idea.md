# Idea: a RAM disk for the PC-1500 on a CE-163X card

**Status:** idea only, 2026-11-10. No code and no plan yet. This page
collects what the PC-1600 ROM does, what the PC-1500 ROM offers, and the
routes that look workable, so that a later plan can start from it.

**Goal.** The PC-1600 ROM can format part of a banked RAM module as a drive
(`INIT "S2:"`, then `SAVE`/`LOAD`/`FILES`/`KILL` on it). The PC-1500(A)
ROM has no file system, but the modern CE-1638 (8 × 16 KB RAM) and CE-163F
(8 × 16 KB RAM + 8 × 16 KB FLASH) cards have the room for one. The feature
should be **software only**: a machine-code driver loaded once, with no
extra ROM on the 60-pin connector.

**Why bother.** The fastest way into a PC-1500 today is CE-158 serial
`CLOAD`: at most 2400 baud, about 70 s for 16 KB. A copy between two banks
of the card takes about 1 s (CE-163F manual, `CALL &C6`). A drive would
hold many named programs and load each at RAM speed.

Sources: the annotated PC-1600 ROM (`PC1600-P1-B3`, `PC1600-P1-B3B`,
`PC1600-P0-B0`) and PC-1500 A04/A03/A01, CE-150 and CE-158 disassemblies
(`Ref` = the research repository), `Ref/PC-1600/PC-1600-Filesystem.md`,
`Ref/PC-1500/Memory-Architecture/PC-1500-Bank-Switching.md`, the CE-163F
manual (V2.0) and the CE-1638 loader sources. Points marked
*(inference)* were read from the code's structure, not traced or measured.

## 1. What the PC-1600 ROM does

The drive code is in ROM bank 3 (`PC1600-P1-B3`), the statements are in
bank 3b.

| Part | PC-1600 | Where |
|---|---|---|
| Window | Disk banks appear at 8000–BFFF. Port 31H b5 = slot, b4 = low bank bit, `OUT (28H)` = 32 KB "vertical bank" on the module. The code runs in ROM at 4000–7FFF and the buffers sit in fixed RAM at C000–FFFF, so the code never switches its own window. | `RDMAPBANK` 4C9E |
| Save / restore | Each call saves port 31H on entry and restores it (and 28H := 0) on exit. | `BANKSAVE` 5005, `BANKREST` 500D |
| Sector | 512 bytes, 32 per 16 KB bank: bank = start bank + sector / 32, address = 8000H + (sector mod 32) × 200H. Copied with `LDIR` straight between window and caller's buffer, no bounce buffer. | `RDSECTSET` 4DA0, `RDSECTIO` 4CCC |
| Header | Sector 0: `55H 80H`, checksum (bytes 00–1F sum to 0), type `81H` at +07, a 19-byte BPB-like geometry block at +08. Media F0H–F8H take their geometry from a ROM table, F9H and up from the header itself (which is how the 512 KB superRAM patch works). | `RDSETID` 4D1E, `MEDIAIDS` 5019, `RDSELECT` 4F16 |
| FAT | One byte per cluster: 00 free, 01–FE next, FF last; byte 0 = media byte; ≤ 254 clusters. Used in place in the window. | `FATGET`/`FATSET` 53A7/53AE |
| Directory | 32-byte MS-DOS-style entries: 8.3 name, attribute, DOS time/date, first cluster, 32-bit size. | `DIRENTRY` 5256 |
| File header | 16 bytes in front of the data: mode (21H BASIC, 10H machine code), length, load and start address. | B3B `SAVEHDR` 6D2E |
| SAVE / LOAD | Stream a line at a time through a 256-byte FCB buffer. LOAD tells binary from ASCII by the first byte. | B3B 6C87 / 6E22 |
| Detection | Header at the start of slot banks 0, 1, 2: signature, type, checksum. A write test at header+7FH finds the write-protect switch. | `RDFINDMOD` 4C03, `RDCHKHDR` 4C44 |
| Sizing | Write the bank number into each bank's first byte, read back to find aliasing, restore. | `RDGETDSKSIZ` 4E15 |
| Consistency | On every select: header checksum, media range, a 256-byte FAT checksum, and a 7-byte drive state compared with the last select (error "changed"). No journal, no second FAT in use. | `RDSELECT`, `RDCHANGED` 4FB9 |
| Split | `INIT "Sx:","F"` formats; `"P",n` / `"M"` then carve a program module or expansion RAM off the front and move the disk header to bank 1 or 2. Only works on an already formatted card (F421 bit 4). | B3B `INITF` 6514, `INITPM` 6464 |

**Worth taking over:** a self-describing header with signature, checksum
and geometry, trusted on mount; sector/bank arithmetic by shifts; a
one-byte-per-cluster FAT; save the bank selection on entry and restore it
on exit; size the card by writing bank numbers; code, stack and buffers
never in the switched window.

**Not needed on the PC-1500:** the track/sector layer (there only to share
the floppy IOCS), the media table, the `"P"`/`"M"` carve-out and its F421
gate, the two-port mapping, the boot sector, DOS dates, the FCB and
128-byte record layer. Format compatibility with the PC-1600 RAM disk buys
nothing either: the BASIC tokens differ and no machine reads the other's
card.

## 2. The card on the PC-1500

- The card fills the window &0000–&3FFF (ME0). A write to &5800+n
  (PC-1500) or &6800+n (PC-1500A) selects bank n through the module's own
  latch; nothing on the connector says afterwards which bank is live
  (`Ref/PC-1500/Memory-Architecture/PC-1500-Bank-Switching.md` §2.2, §9).
- BASIC lives **in** that window: program from &00C5 (or `NEW n`) of the
  active bank, running on into the built-in RAM at &4000; variables down
  from &47FF (&57FF on the PC-1500A). A bank switch therefore has to save
  and restore the BASIC pointers (&7865–&786A, &7864, &7899/&789A), which
  the cards' firmware does.
- Both cards' firmware already uses the key trick: **the same stub sits at
  the same address in every bank** (CE-1638 &00C5–&0104, CE-163F
  &00C5–&0111). Code running in the stub can switch banks under itself and
  carry on with the next instruction, because it is identical in the new
  bank. The CE-163F stub dispatches into its management bank 8 this way
  (copy, format, flash).
- The power-on scan writes &5A/&A5 to `xx00` of every page &00–&6F,
  &5800/&6800 included, so after a reset bank 0 is probably selected
  *(inference)*. The driver must never assume a bank; it reads the
  firmware's bank byte (CE-1638 `PEEK &D4`, CE-163F `PEEK &E2`).

## 3. How the PC-1500 loads programs

| Path | How the bytes land | Pointer fix-up |
|---|---|---|
| CE-150 `CLOAD`/`CHAIN`/`MERGE` (CE-150 B8F9, BB6A, B994) | `FILETRSF` (VMJ AA) writes straight to `BASPRG_ST` (MERGE: `BASPRG_END`+1), no buffer; 40-byte header at &7B60 (name, start, length, entry) | BASPRG_END = last byte, BASPRG_EDT = start, `DEL_DIM_VAR_1` (D09C) drops overwritten arrays (B9B9–BA0F) |
| CE-158 `CLOAD`/`MERGE`, `M`, `r`, `a` suffixes (CE-158-LOW 90BB–90DE) | UART loop writes straight into RAM (9AA7); `a` goes line by line through the tokenizer | FF end marker, BASPRG_END, D09E, BASPRG_EDT (927F–928A) |
| `CLOAD M` on either | after loading, jump to the entry address unless &FFFF (CE-150 B94C, CE-158 9272) | – |

A drive only has to do what these do: copy the image to `BASPRG_ST` (or
the end, for MERGE), then the same three pointer steps. A tokenized PC-1500
program is position independent (line searches, no addresses), so a file
saved from one bank loads into any other.

**The device layer.** The ROM already routes the cassette words through a
table pointer:

- `OPN_TBL` (&79D1) holds (PV << 7) | (table page >> 1). `TOK_PROCESS`
  (FA89) looks an &F0xx token up there first, then steps down through the
  other tables (`DEC_OPN` FA58); ERROR 27 when none has it. `OPN "name"`
  sets it by a module's header name (E457–E49E).
- `TOK_PROCESS` accepts a table at **any 2K boundary with &55 at xx00,
  RAM included**. BASWORD (a RAM-resident keyword manager, 1986) relies on
  this: `POKE &79D1,&04` makes its table at &0800 the OPN table
  (`pc1500/notes/PC-1500-A0x-Revision-Differences.md`, *BASWORD needs the
  A03 keyboard hook*).
- What the ROM can't do with a RAM table is **tokenize** typed text: the
  tokenizer's table search (E4A8) only looks at &8000–&B800. Text becomes
  a token only through a table there (CE-150, CE-158) or a keyboard driver.
- `PRINT#n`/`INPUT#n` with n ≥ 2 go to the module whose table number is n
  (+10 / +0D), again only in &8000–&BFFF (E4CE–E511).

**RAM hooks** (A03/A04; A01's keyboard hook is broken):

| Hook | Effect | Survives |
|---|---|---|
| &79D4 = &55 → &785B/&785C | every key wait goes to external code (bit 0 = PV) | cleared at ON after the OFF key; kept after auto power-off |
| &79DA = &55 → &79DB/&79DC | LH5811 interrupt sources | same |
| &79D1 `OPN_TBL` | the token table looked at first | reset to &60 at ON after the OFF key |
| `CALL addr,var` (C863) | machine code with a numeric argument | – |

(Reset E034–E086: ON after the OFF key clears &79D2–&79DE and resets
`OPN_TBL`; after auto power-off it skips that. The cards' OFF-key
intercept probably powers down through the auto-off path for that reason
*(inference)*.)

## 4. Routes

Ordered by effort; each builds on the one before.

### A. A file store behind `CALL`

Machine code reached through `CALL`, the name in a fixed string variable
(say `A$`), a result code in the numeric variable:

```
A$="GAME":X=1:CALL &xxx,X     ' save the current bank's program as GAME
A$="GAME":X=2:CALL &xxx,X     ' load GAME into the current bank
X=3:CALL &xxx,X               ' list the directory on the LCD
A$="GAME":X=4:CALL &xxx,X     ' delete
```

Works on every ROM revision, with or without CE-150/CE-158. Reserve keys
make it bearable (the CE-163F manual already sets one up for bank
switching).

### B. `CLOAD` / `CSAVE` / `MERGE` / `CHAIN` redirected to the card

A RAM token table (&55 header on a 2K boundary) that defines the cassette
words with their usual codes (&F089 CLOAD, &F095 CSAVE, &F08F MERGE,
&F0B2 CHAIN). `POKE &79D1` to it, and `CLOAD "GAME"` loads from the card
instead of the tape or serial line; `OPN` (or `OPN "COM"`) switches back.

- With a CE-150 or CE-158 attached the words are typed as usual: their
  table tokenizes them, the redirected `OPN_TBL` executes them.
- Without either, the base ROM's DEF-key table (`DEFKEY_TBL`, C36B) still
  gives DEF+I = CLOAD, DEF+U = CSAVE, DEF+O = MERGE as tokens *(how those
  keys produce the token was not traced)*.
- `OPN_TBL` is reset by ON after the OFF key, so the driver has to be
  re-armed then: by a reserve key, or through the keyboard hook if it is
  kept alive as in §3.
- Programs that use `CLOAD`/`CHAIN` keep working unchanged and chain from
  the card.

### C. Own keywords through the keyboard hook

`FILES`, `KILL`, `DSKF` and the like need a tokenizer: the BASWORD way, a
keyboard driver on &79D4/&785B that turns the words into &F0xx tokens on
ENTER, plus the same RAM table for execution and `LIST`. A03/A04 only
(`PEEK &E2B9` = 56). The most comfortable, and the most code.

(A real keyword module at &8000–&BFFF, found by the tokenizer and armed
again by its +0A code at every power-on, would avoid all re-arming. It
needs RAM decoded in Y2, i.e. hardware, so it is out of scope here.)

## 5. Where the driver lives

The window rule: code, stack and ROM-called handlers must never be in a
bank that gets switched out.

- **Driver bank.** The driver sits in one bank of its own (CE-163F: the
  management bank 8 already plays this role, or a FLASH bank 9–15; CE-1638:
  bank 7). A small entry in the per-bank stub (same address in all banks)
  saves the BASIC pointers, switches to the driver bank, calls in, and
  switches back. That is how the CE-163F's own copy and format commands
  already reach bank 8.
- **Moving data between banks.** Program bank and disk bank share the
  window, so data goes through a buffer outside it: either a copy loop in
  the per-bank stub, or a short loop copied into system RAM (&7600–&7FFF)
  for the duration of the command. Which system RAM bytes are free while a
  `CLOAD` runs (the cassette header buffer &7B60–&7BAF is one candidate) is
  still to be checked. Even an 80-byte buffer means only about 200 bank
  switches for 16 KB.
- The part of a BASIC program that sits in the built-in RAM (&4000 up) is
  outside the window and can be copied directly.
- **Coexisting with the card firmware.** Disk banks keep the firmware stub
  (&00C5–&0111 on the CE-163F), so `CALL &E3` and the OFF-key intercept
  still work in every bank; the disk area starts after it. The cards' copy
  command and pointer save/restore stay the user's way to switch banks.
- **Getting it in.** Once, via CE-158 `CLOAD M` with auto-start (as the
  CE-1638 loader already does with SharpDataExchange `put
  --run-address`), which installs the driver into its bank. The card is
  battery-backed (CE-163F FLASH even without battery), so later only the
  arming step runs.

## 6. A file system that fits

Smaller than the PC-1600's, because LH5801 code space is the scarce part
and RAM copies are fast:

- **Header** in the first disk bank: signature, checksum, version, first
  and last disk bank, written by a format command and trusted on mount (the
  PC-1600 idea without the table).
- **Contiguous files**, packed one after the other across bank boundaries
  (disk offset → bank = offset >> 14, address = offset & &3FFF, skipping
  the stub area). Delete marks the entry; a "squeeze" closes the gaps by
  copying. Far less code than a FAT, and the right shape for FLASH, which
  erases in 4 KB sectors: append to FLASH, squeeze into RAM and write back.
  A one-byte FAT with 1 KB clusters (≈ 120 clusters on 8 banks) is the
  fallback if fragmentation turns out to matter.
- **Directory entry = the CE-150 tape header fields**: 16-byte name, mode
  (BASIC, machine code, reserve area), start, length, entry address. Then a
  file is exactly what `CSAVE`, `CSAVE M`, `CSAVE r` put on tape, and the
  host side (libsharpdx / SharpDataExchange) can read and write disk
  images with the formats it already knows.
- **Checks on mount**: header checksum, directory entries inside the disk,
  end of the last file inside the disk. Write protection and the "card
  pulled while armed" case as on the PC-1600: detect, refuse, report.

## 7. In Calc-U-1600

The emulator already has both cards (`ce1638.card.yaml`,
`ce163f.card.yaml`) and the CE-158, so a driver can be developed and tested
there: install via the serial path, run the `CALL` and `CLOAD` routes on
A01/A03/A04 and the PC-1500A, and compare with the real card. A disk image
is then simply the card file; the INSPECTOR could list its directory like
the PC-1600 RAM-disk item in TODO.md.

## Open points

- Which bank the card shows after power-on, and whether the CE-163F's
  OFF-key intercept keeps `OPN_TBL` and &79D4 alive (measure on the unit).
- Free system RAM for a transfer buffer and a trampoline while `CLOAD`
  runs.
- The DEF-key tokens without CE-150/CE-158: do `CLOAD`/`CSAVE` typed that
  way list and run through a RAM `OPN_TBL`?
- Whether machine code may run from a CE-163F FLASH bank (the card notes
  only say programs should not run from FLASH, which for BASIC is about
  editing).
- The CE-1638's latch on the PC-1500A (&6800) and its firmware's bank byte
  layout, which is less documented than the CE-163F's.
