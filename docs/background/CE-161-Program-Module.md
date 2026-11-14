# CE-161: a RAM module that becomes a program module

The CE-161 is 16 KB of battery-backed static RAM for the PC-1500 with a
write-protect switch. It contains no ROM and doesn't remap anything. Its
"ROM mode" comes from the PC-1500 system ROM: at power-on the ROM finds a
page it can't write to and that starts with a module header, and from then
on treats it as a program module. This page explains how that works and
what the manual's `POKE` does. The emulator models it as `write-protect:`
in [ce161.card.yaml](../../Qt6/resources/cards/ce161.card.yaml).

Sources: CE-161 Instruction Manual (pp. 14–27, 37), CE-161 Service Manual
(schematic, p. 3), and the annotated A04 system ROM disassembly, whose
labels are used below. A01 and A03 run the same scan; only its result
table sits elsewhere.

## Hardware

- Eight 2 KB RAMs (two 8 KB HM6264 in the flat version) fill the Y0 window
  &0000–&3FFF. An on-module TC40H138 picks the chip from AD11–AD13; Y0 on
  G2A gates it to the window, DME0 on G1 to ME0 cycles.
- **Power:** the RAMs and the '138 run from a diode-OR of V<sub>GG</sub>
  (1SS98) and a CR2032 (DAN202), buffered by 1 µF. Pulled from the
  computer, the module keeps its contents; nothing on the board erases
  them.
- **Deselect when unplugged:** AD11–AD13, Y0 and DME0 have 1.2 MΩ
  pull-downs. Without the computer DME0 (G1) sits low, so all chip enables
  stay high and the RAMs hold their data in standby.
- **Power-up lock:** G2B hangs on 0.1 µF from V<sub>CC</sub>, with 1.2 MΩ and
  a DAN202 clamp to ground. While V<sub>CC</sub> rises it pulses G2B high
  and deselects every chip, so the switch-on transient can't write; with
  V<sub>CC</sub> steady G2B is low and the decoder works.
- **Output gating:** contact 38, OD (output disable), drives the chips'
  output control (ŌD on the 2 KB chips; on the flat version's HM6264
  probably ŌE). A chip drives the data bus only when OD allows it, so a
  write cycle to a protected chip, which sees "read", can't fight the CPU
  on the bus.
- The **Exclusive Readout Area Set Switch** sits in the RAMs' R/W line
  (contact 39). *Release*: R/W passes through, normal RAM. *Setting*: the
  switch is open and 3.3 MΩ pulls the chips' R/W to the backed-up
  V<sub>DD</sub>, so they always see "read": writes are lost, reads still
  work.
- **Open point:** the 2 KB-chip schematic shows a second, unlabelled input
  that joins the chips' R/W *after* the switch. OD is drawn separately (to
  ŌD), and a host signal driving that node would defeat the write
  protection, which the manual says works. Probably a drafting error,
  perhaps a leftover of the OD line; only a measurement on a real module
  would settle it.

The manual's "the program and data … are cleared when it is
disconnected" (RAM mode) is about use, not hardware: the bytes stay, but
the program also runs through the built-in RAM and its pointers live in
the PC-1500, which asks for `NEW` after the memory layout changes.

The 18 KB the manual speaks of are just the CE-161 at &0000–&3FFF plus the
PC-1500's own 2 KB at &4000–&47FF, back to back.

## What the system ROM does at power-on

`RESET_9` (E098) walks the pages &00–&6F. For each page it writes 5A and
A5 to `xx00`, reads them back, and restores the byte:

- **Writable page:** the first one becomes `RAM_ST_H` (&7863). The first
  non-writable page after the RAM block becomes `RAM_END_H` (&7864).
- **Not writable, and `xx00` = &55:** a module header (`RESET_13`/`RESET_14`).
  Only the first one counts:

| Offset | Meaning | Ends up in |
|---|---|---|
| +0 | &55 | – |
| +1 | page override (&FF = this page) | `ST_ROM_MOD` &7860 = module page |
| +2 | program start, page offset (bit 7 set = no program) | &7861 = page + offset |
| +3 | program start, low byte | &7862 |
| +4…+6 | not read here | – |
| +7 | bit 7 = 1: lines can be listed / edited; 0: ERROR 31 | read by `LAST_LINE_2_INBUF` (D277) |

The results are compared with the saved &7860–&7864. A difference sets
`ARV` bit 2, and the computer shows `NEW0? :CHECK`.

While the switch is in *Release*, page &00 is writable, so the header is
ignored and the module is ordinary RAM.

## The `POKE`

`POKE 0,85,0,1,0,0,0,0,255` writes `55 00 01 00 00 00 00 FF` to
&0000–&0007: module at page &00, BASIC program at **&0100**, listable.

The manual starts with `NEW 256` so that the program sits where this
header says. The ROM doesn't need &0100: `NEW` accepts any address from
`RAM_ST`:C5 up (`BCMD_NEW`, C80A), nothing uses &00C5–&00FF, and Sharp's
own CE-502B mask-ROM module starts its program at &00C5 (header
`55 00 00 C5 40 00 00 FF`). `NEW 0` with `POKE 0,85,0,0,197,0,0,0,255`
behaves the same as the manual's variant in every check (RUN, labels,
LIST, MEM, OFF/ON) and gives 59 more bytes. The manual's CE-153 variant
loads that board's machine code at &0100 (`CALL &100`, `&104`, `&108`) and
starts BASIC at &052A (`POKE 0,85,0,5,42,…`); a page-aligned start is the
likeliest reason for &0100, but the manual doesn't say.

The manual's fill program (lines 65273–65278) fills the module from the
program's end marker (`STATUS 2`) to &3FF8 with &FF and writes
`FE FF 03 F1 8E 0D FF` at &3FF9: the line `65279 END` plus the end marker,
ending on the module's last byte. Capacity: 16384 − 256 (reserve +
header) − 125 (the fill program, which stays) − 7 − 1 = 15995 bytes.

## Memory maps (PC-1500)

**Release, after `NEW 256`:** MEM = &4800 − &0100 − 1 = 18175.

| Address | Contents |
|---|---|
| &0000–&0007 | header bytes (ignored while writable) |
| &0008–&00C4 | reserve area (`RSRV_MEM_START`, CE9F: `RAM_ST`:08) |
| &00C5–&00FF | unused |
| &0100 → | BASIC program, across the module/built-in boundary |
| ← &47FF | variables, down from `RAM_END` |

**Setting, after OFF/ON and `NEW 0`:** the scan finds pages &00–&3F not
writable and takes the header; RAM is &4000–&47FF only.

| Address | Contents |
|---|---|
| &0000–&0007 | header → `ST_ROM_MOD` = 00, program at &0100 |
| &0008–&00C4 | reserve area, now read from the module (`RSRV_MEM_START` uses `ST_ROM_MOD`) |
| &0100 – `STATUS 2` | the protected program, fill program included |
| … – &3FF8 | &FF |
| &3FF9–&3FFF | `65279 END` + end marker |
| &4000 → | new RAM program: with a module, `NEW 0` keeps the low byte 00 instead of C5 |
| ← &47FF | variables; RAM capacity 2047 |

The PC-1500A has RAM to &57FF, so `RAM_END` is &58 there; the manual
covers the PC-1500 only.

## Program modules in general

**Layout.** Sharp's program modules follow one layout, the mask-ROM
CE-502B as well as a CE-161 prepared by its manual (both have the same
closing bytes at &3FF9–&3FFF):

| Address (module at page &00) | Contents |
|---|---|
| &0000–&0007 | Header: &55, module page, program start (page offset, low byte), 3 bytes not read by the scan, list flag |
| &0008–&00C4 | Reserve area: the DEF-key labels and key strings |
| &00C5 or &0100 → | The tokenized BASIC program, ending in the usual &FF end marker |
| … – &3FF8 | &FF fill |
| &3FF9–&3FFE | `FE FF 03 F1 8E 0D` = line `65279 END` |
| &3FFF | &FF, its end marker |

- **Program start:** the CE-502B starts at &00C5, the CE-161 manual at
  &0100 (&052A with the CE-153's machine code at &0100). The ROM accepts
  any start from &00C5 up; why Sharp chose &0100 for the CE-161 is unknown.
- **&FF fill:** needed. It lets the block search skip the gap to a RAM
  program at &4000 (measured, see below).
- **`65279 END`:** present in the CE-502B mask ROM and written by the
  CE-161 procedure, but its purpose is unknown. On A01, A03 and A04, &FF
  up to &3FFF without this line worked the same.

**How the system ROM treats one.** None of this is CE-161-specific: the ROM treats every module the scan
finds (`ST_ROM_MOD` ≠ FF) the same way, a protected CE-161 and Sharp's
mask-ROM modules alike (checked with the CE-502B: module at page &00,
program &00C5, RAM &4000–&47FF, `NEW 0` → &4000).

- **Reserve area:** `RSRV_MEM_START` (CE9F) returns `ST_ROM_MOD`:08
  instead of `RAM_ST`:08, so the reserve keys are read from the module.
  They can't be reprogrammed (`RSRV_MEM_START` hands back error code 24
  for that case).
- **RAM BASIC:** `BCMD_NEW` (C80A) sets `NEW 0` to `RAM_ST`:00 instead of
  `RAM_ST`:C5 — the &C5 bytes in front are only for the reserve area,
  which now lives in the module.
- **Variables:** as always, down from `RAM_END`, in the built-in RAM.
- **Program start:** `RUN` and line searches start at the module's program
  (`PRG_SA_2_XREG`); labels reach the RAM program, line numbers don't
  (next section).
- **Plugging in or out:** the scan result changes, so `ARV` bit 2 asks
  for `NEW 0`, which moves RAM BASIC between `RAM_ST`:00 and `RAM_ST`:C5.
  What the built-in RAM held is lost; the module keeps its program and
  reserve keys.

The research repository has the same as a reference section:
[Ref/PC-1500/Memory-Architecture/PC-1500-Address-Decoding.md](https://github.com/tinue/Sharp1500-1600-Ref/blob/main/PC-1500/Memory-Architecture/PC-1500-Address-Decoding.md)
§5.6. The user-level version, with `MEM` figures for the PC-1500 and PC-1500A,
is in the [User Guide](../User-Guide.md#program-modules-on-the-pc-1500-where-basic-and-the-reserve-keys-go).

## Module program and RAM program as one

`PRG_SA_2_XREG` (VMJ 12, DF93) returns the module's program start whenever
&7861 bit 7 is clear, so `RUN` and line searches start in the module.
**Labels** reach both programs: `GOTO "A"` / DEF finds a label in the
module or in the RAM program. **Line numbers** don't cross over: a direct
`GOTO 500` or `LIST 500` for a RAM line gives ERROR 11, and so does
`GOTO 10` from a RAM line into the module. That matches the manual
(p. 37): a program in the remaining RAM must be a label ("definable")
program, started with DEF or `RUN "label"`.

**Why the &FF fill.** When a search reaches a block's end marker it
doesn't stop: it skips &FF bytes (the `cin` loop in `LINE_SEARCH`, bounded
by `BASPRG_END`) and reads the first other byte as the start of the next
block. Between the module program's end and &4000 only &FF is certain to
be skipped. Anything else is read as a block of lines, and the search
loses its way before it reaches the RAM program. Measured (A04 ROM,
PC-1500, module protected, RAM program with a label at &4000):

| Contents after the module program | Module labels, `RUN`, `LIST` | Label in the RAM program |
|---|---|---|
| &FF to &3FF8 + `65279 END` (manual) | OK | OK |
| &FF to &3FFF, no `END` line | OK | OK |
| old contents of the card | OK | ERROR 11 |
| zeros, gap a multiple of 3 | OK | OK |
| zeros, gap not a multiple of 3 | OK | ERROR 11 |

Zeros only work by luck: `00 00 00` reads as an empty line 3 bytes long,
which lands exactly on &4000 only if the gap divides by 3. The module's
own program needs no fill. The `65279 END` line made no difference in
these tests; its purpose remains open (perhaps a defined last line, so
execution can't run on into the RAM program). The manual only says a RAM
program can't be run without the fill program.

## In the emulator

- `write-protect: protected|unprotected` on regular content is the
  switch; the position is part of the card file. While protected, every
  write is refused, host pokes included ([Decisions.md](Decisions.md)).
- Name & Save of the template offers "Write-protect"; presets use
  `saveas: live protected slot-1:<name>`. The live card is protected at
  once; the ROM sees it at the next OFF/ON, as in the manual.
- `Core/tests/ce161_tests.cpp` runs the whole procedure against the A04
  ROM on the PC-1500 and PC-1500A.
