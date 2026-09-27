# PC-1600 loaders follow the current MODE and program area

## Context

**File ▸ Load BASIC Program…**, **File ▸ Load Machine Code…**, a preset's
`format: basic-binary` / `format: binary` and the debugger's Build & Load put a
program straight into memory. How a real machine would have received it (cassette
through the CE-1600P or CE-150, `COM1:`, the CE-158, a floppy) doesn't matter; only
the result in memory does. For machine code the loader also advises the `NEW` that
protects it and types the command that starts it, as a convenience.

On the PC-1600 the result depends on the MODE the machine is in and on the selected
program area (`TITLE`). Today the loaders ignore both: BASIC always goes to S0, and a `.bas` is always tokenized with the PC-1600 keyword table, a
CE-158-header file is always refused, and headerless code is always taken as Z-80
code. The ROM analysis behind this plan is in SharpPC1500Reference
`PC-1600/PC-1600-Load-Save-Matrix.md`.

## Principles

1. **The current MODE decides.** The loader reads `BMODE` (F1BCH) bit 6 at load time:
   0 = MODE 0 (native), 1 = MODE 1 (PC-1500 compatible).
2. **Never switch MODE.** The real machine doesn't either; it shows an error when a
   program doesn't fit. The preset owns the mode (docs/Decisions.md, "Preset loading").
3. **No guessing what a file is.** No PC-1500-vs-PC-1600 detection for BASIC, no
   Z-80-vs-LH5801 detection for machine code. A header, where there is one, is checked
   against the MODE; a mismatch is an error.
4. **The periphery is never consulted.** It is only the way a program would have
   arrived.
5. **Placement comes from the live work area** (ADTBL, descriptors, `TITLE`), which
   the ROM has already set up for the current MODE — MODE 1 narrows it to one program
   area (`PC15MAP`, P0-B0 1676H). MODE 1 doesn't need a module; with one, it is at most
   one bank. The loader follows whatever the ROM set up and has no MODE 1 special case
   for placement.
6. **The selected program area decides where BASIC goes.** A BASIC load targets the
   area `TITLE` (F1D5H) names: S0, or the S1/S2 program module after `TITLE "S1:"` /
   `"S2:"`. Never change `TITLE` either.

## Program area (`TITLE`)

What the ROM does (SharpPC1500Reference `PC-1600-CPU-LH5803-Compat.md` §4,
`PC-1600-Work-Area-Map.md` §4.5):
- `TITLE "Sn:"` (`SELPRG`, P0-B0 1EEFH) selects S0, or S1/S2 if that slot holds a
  program module (else error 101). The ROM's `LOAD` then writes into that area
  (`PRGRANGE`, rom3b 71B3H) and finishes it per area (`LOADEND` 70E1H: S0 pointers
  F867/F02C, or the S1/S2 descriptor end triple and module header +5/+6).
- **`MODE 1` ignores the previous `TITLE`** and picks the area itself (`PC15MAP`):
  with S0 in internal RAM only, a one-bank program module in S1, else one in S2, else
  S0; with one module bank in S0, S0 unless that bank is only part of the window and a
  one-bank program module sits at `ADTBL` entry 4. It sets `TITLE` to its choice and
  hides the other slots (SxMTb := FFH), so `TITLE "Sx:"` to them fails with error 101.
  So after `TITLE "S1:"` + `MODE 1`, a PC-1500 program goes to S1 only if S1 holds a
  one-bank program module; `TITLE "S2:"` + `MODE 1` with program modules in both slots
  ends up in S1.
- `MODE 0` rebuilds the descriptors and resets `TITLE` to S0.

For the loaders this means: read `TITLE` and load into that area, in both MODEs. The
ROM's choice in MODE 1 is already in `TITLE`, so the loader needs no MODE 1 rule of
its own. For machine code, `TITLE` decides the default address offered for a
headerless file (the start of the selected area) and the `NEW` in the advice
(`NEW "Sn:",size` for that area in MODE 0); the code itself goes where its address
says.

## BASIC

| Input | MODE 0 | MODE 1 |
|---|---|---|
| `.bas` listing | tokenize with the PC-1600 table | tokenize with the **PC-1500** table |
| PC-1600 transfer file (16-byte header) | load | load (the ROM's `LOAD` has no MODE check) |
| CE-158 transfer file (27-byte header) | error: PC-1500 program, needs MODE 1 | load |

- Tokenizing failure is a load error; nothing is written. The PC-1500 tokenizer
  (libsharpdx device `pc1500`) already refuses non-7-bit-ASCII text, so
  `10 PRINT "äöü"` loads in MODE 0 and is refused in MODE 1 — it is clearly a
  PC-1600 program.
- The keywords whose meaning depends on the table are the ones the PC-1600 renamed:
  `CALL`, `PEEK`, `POKE`, `PEEK#`, `POKE#`, `LINE`, `LCURSOR`. In MODE 1, a PC-1500
  listing's `CALL` becomes the PC-1500 `CALL` token, which the PC-1600 lists as `XCALL`
  — the same bytes a `CLOAD` of that program produces.
- **Deliberate difference from the ROM:** the ROM tokenizes typed or ASCII-`LOAD`ed
  text with the PC-1600 names even in MODE 1 (the TRM: rename `CALL` → `XCALL` by hand).
  The loader treats a listing loaded in MODE 1 as a PC-1500 program instead, the way a
  PC-1500 `CSAVE` + PC-1600 `CLOAD` would. So PC-1600-only keywords (`TITLE`,
  `LOCATE`, …) are refused in MODE 1 although the Z-80 interpreter would run them.
  To be recorded in docs/Decisions.md when implemented (step 5).
- The asymmetry with the transfer files (a PC-1600 binary loads in MODE 1, a PC-1600
  listing doesn't) is kept: for binaries it is what the ROM does.

## Machine code

| File | MODE 0 | MODE 1 |
|---|---|---|
| PC-1600 header (Z-80 bank + address) | load | load (`BLOAD` works in MODE 1) |
| CE-158 header (LH5803 address) | error: PC-1500 file, needs MODE 1 | load at the LH5803 address |
| No header | dialog asks for a **Z-80 address**; note "Z-80 code assumed" | dialog asks for an **LH5803 address**; note "LH5801 code assumed (PC-1500 address space)" |

- LH5803 addresses map to the Z-80 through the existing `LoadOptions::lh5803` path
  (`Core/MachineCodeFile.hpp`; its 0000–7FFF = the Z-80's 8000–FFFF, used by the
  debugger today). This is the one `+0x8000` helper the program-loading TODO asks
  for; the BASIC loader, typer and placement should use it too.
- **Range check:** LH5803 7C00H–7FFFH is the PC-1600's system work area (Z-80
  FC00H–FFFFH). The PC-1500A's user ML area is there, which is one reason the manual
  calls the PC-1600 compatible with the PC-1500 only, not the PC-1500A (Operation
  Manual App. H). Refuse loads into it with that explanation instead of overwriting
  the work area.
- **Advice after loading** depends on MODE and on the code's CPU:
  - MODE 0: `NEW "Sn:",size` for the selected area to reserve (today always
    `"S0:"`), `CALL #bank,&addr` to start.
  - MODE 1, LH5801 code: `NEW &addr` the PC-1500 way (MODE 1 only, rom3b 4351H: an
    LH5803 address at or above the S0 base + C5H and below the variable area), start
    with `XCALL &addr`.
  - MODE 1, PC-1600-header file: Z-80 code, so still `CALL #bank,&addr`; the MODE 1
    `NEW &addr` still protects it when it lies in the program area.
  - Auto-run addresses from a header keep today's behaviour (the command is typed,
    never pressed).

## Implementation steps

1. **A MODE query** on `PC1600Machine` (or `PC1600Memory`): `bool mode1() const`,
   reading F1BCH bit 6 without side effects. One place, used by every loader.
2. **BASIC**: `loadBasicProgramLiveOn` (Qt6/app/PresetController.cpp) and the preset
   runner's `format: basic-binary` pick the `basic::TransferModel` for
   `readBasicProgramSource` from the machine: PC-1500/1500A → PC1500; PC-1600 →
   PC1600 in MODE 0, PC1500 in MODE 1. The payload goes through the PC-1600
   `loadBasicBinaryPayload` in both MODEs. `loadBasicBinaryProgram` accepts a CE-158
   image on the PC-1600 in MODE 1.
3. **Machine code**: `machinecode::plan()` / `planLoad()` / `headerMismatch()` take the
   MODE: a CE-158 file on a PC-1600 is fine in MODE 1; headerless code in MODE 1 sets
   `lh5803`. `advice()` gets the MODE and the CPU. `MachineCodeLoadDialog` shows the
   address-space note. New `LoadError` for the work-area range.
4. **Presets**: the same checks through the same functions, so a preset that loads a
   PC-1500 program switches to MODE 1 (`- type:` steps) before its `program:` step.
   No preset gets a workaround (docs/Decisions.md, "Fix the loader, not the preset").
5. **Program area**: the BASIC loaders target the `TITLE` area. S1/S2 need the
   descriptor-based placement and the ROM's `LOAD` finish for that area — the
   program-loading TODO chapter (mirror `LOADEND`), done as part of this step, on top
   of the shared `+0x8000` helper from step 3. Machine code: default address and `NEW`
   advice from the `TITLE` area.
6. **Docs**: User Guide §4 and §6 (the MODE and `TITLE` rules, the dialog note, the
   MODE 1 advice), docs/Decisions.md entry for the listing rule.

## Tests

- `.bas` with `CALL &4000`: MODE 0 → the PC-1600 `CALL` token; MODE 1 → the PC-1500
  token, `LIST` shows `XCALL`.
- `10 PRINT "äöü"`: loads in MODE 0, refused in MODE 1, memory unchanged.
- A PC-1600-only keyword (`TITLE`) refused in MODE 1.
- CE-158 BASIC image: refused in MODE 0, loaded in MODE 1; `--dump-basic` equals the
  typed program.
- MODE 1 placement: no module, and a ≤16 KB module (e.g. CE-155) — the program lands
  where the ROM's own `LOAD` puts it (work-area diff typed vs loaded).
- Headerless ML in MODE 1 at an LH5803 address: bytes at +8000H; `XCALL` runs it.
- LH5803 7C01H refused with the work-area message.
- `TITLE "S1:"` with a program module in S1 (MODE 0): the program lands in S1, the
  S1 descriptor and header match the ROM's `LOAD` (work-area diff); S0 unchanged.
- `TITLE "S1:"` + `MODE 1`, S1 a one-bank program module: `TITLE ?` gives 1 and the
  PC-1500 program lands in S1. With a multi-bank program module in S1 instead: `TITLE`
  is 0 and the program lands in S0.
- `TITLE "S2:"` + `MODE 1` with program modules in both slots: `TITLE ?` gives 1, the
  program lands in S1. (Checks the ROM behaviour above on the emulator as well.)
- The advice strings per MODE/CPU.
- The existing PC-1500 and PC-1600 MODE 0 loader tests unchanged.

## Out of scope

- Whether all shared tokens really have the same values on both machines (matrix
  open question 2). The MODE 1 rule assumes they do; a table comparison would settle
  it.
