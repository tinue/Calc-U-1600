# Loader matrix

A one-time verification of the fast loaders against the ROM: each program
is loaded twice, once by the ROM over a serial line and once by the
emulator's loader, and the memory is compared afterwards. This directory is
not part of `tools/run_tests.sh` or CoreTests.

- [PC-1500 / PC-1500A](#pc-1500--pc-1500a): 80 cells, CE-158
- [PC-1600](#pc-1600): 250 cells, COM1: and CE-158

# PC-1500 / PC-1500A

Each program is loaded twice:

- **ROM:** `SETCOM 1200`, `SETDEV CI`, then `CLOAD` (BASIC) or `CLOAD M`
  (machine code) over the CE-158. The serial peer sends what `sde put`
  sends: the 27-byte CE-158 header, a 300 ms pause, then the payload.
- **Loader:** the same machine with `SETCOM`/`SETDEV` typed, then a preset
  `program: file:`. BASIC gets the `.bas` listing, so the in-process
  tokenizer is checked as well. Machine code gets the `.ce158.bin`.

The CE-158 is attached in both runs.

## Running it

```sh
dev/loader-matrix/run_pc1500.sh            # build, generate, run all 80 cells (about 1 minute)
python3 dev/loader-matrix/edge_pc1500.py PC-1500 none 1850   # BASIC capacity edge
```

Everything goes to `headless/loader-matrix/`:
- the harness binary
- `progs/`, the generated programs
- `manifest.tsv`
- `logs/`, one log per run
- `results.md`, one row per run

| File | What it does |
|---|---|
| `pc1500_loader_matrix.cpp` | The harness. One run per call (`--model --card --kind --stream --loader-file [--expect-refuse]`), or `--probe` for BASPRG_ST / RAM_END. In-process, through `applyPC1500Preset`. |
| `build.sh` | Builds the harness. |
| `gen_pc1500.py` | Probes each model and card, then writes the programs with `sde convert`. BASIC is a deterministic mix of statements; machine code is LFSR bytes loaded at BASPRG_ST+1, so NEW0's end mark stays, with no auto-run. |
| `run_pc1500.sh` | Runs the manifest and writes the results. |
| `edge_pc1500.py` | Byte-exact BASIC listings around the capacity limit. |

**`sde put` sends the `sde convert` bytes.** sde can't open a PTY for a
PC-1500 device, so this was checked in sde's source instead
(`transfer::build_put`):
- a headered file goes out as-is
- a `.bas` is tokenized with the same `Wire` segment marker as `convert`
- `put --dry-run` reports the same byte count and `header_len=27`

## Pass criteria

A cell passes when all of these hold:
- Both loads succeed, with no `ERROR` on the display.
- In both runs, the payload sits where it belongs (from BASPRG_ST for BASIC,
  at the header address for machine code).
- User RAM `[RAM_ST·256, RAM_END·256)` and the pointer block &7860–&786F are
  byte-identical between the two runs.

In the OVER tier (free + 256 bytes) both paths must refuse.

Differences in the rest of system RAM are listed per run for information.
They are the same in every run:

| Address | What it is | Why it differs |
|---|---|---|
| &7855 | CE-158 CR/LF register | `CLOAD` over RS-232C sets it |
| &7A00–&7A07, &7A28–&7A2B | FP accumulator, scratch register | used by `CLOAD` |
| &7B0B–&7B0C | auto power-off counter | time elapsed |
| &7B60–&7BAF | output buffer | holds the typed `CLOAD` line |
| &784C–&784D | not in the symbol tables | `CLOAD` leaves &70E2 there (seen with PC-1500 + CE-155 only; elsewhere the value happens to match) |

On the plain PC-1500, &7C00–&7FFF shows the same bytes again: it aliases
&7800–&7BFF.

## Matrix and results (2026-10-03)

Free bytes (MEM after NEW0) and the program area:

| Model | Card | BASPRG_ST | RAM_END | Free |
|---|---|---|---|---|
| PC-1500 | none | &40C5 | &4800 | 1850 |
| PC-1500 | CE-151 | &40C5 | &5800 | 5946 |
| PC-1500 | CE-155 | &38C5 | &6000 | 10042 |
| PC-1500 | CE-161 | &00C5 | &4800 | 18234 |
| PC-1500A | none | &40C5 | &5800 | 5946 |
| PC-1500A | CE-151 | &40C5 | &6800 | 10042 |
| PC-1500A | CE-155 | &38C5 | &7000 | 14138 |
| PC-1500A | CE-161 | &00C5 | &5800 | 22330 |

Sizes are payload bytes. The tiers are S ≈ 64, M ≈ ⅓ free, L ≈ ⅔ free,
XL = free − 100, and OVER = free + 256. ✅ = PASS, ⚠️ = differs (see below).

| Model | Card | BASIC S | BASIC M | BASIC L | BASIC XL | BASIC OVER | ML S | ML M | ML L | ML XL | ML OVER |
|---|---|---|---|---|---|---|---|---|---|---|---|
| PC-1500 | none | 50 ✅ | 616 ✅ | 1204 ✅ | 1733 ✅ | 2100 ✅ | 64 ✅ | 615 ✅ | 1232 ✅ | 1749 ✅ | 2105 ⚠️ |
| PC-1500 | CE-151 | 50 ✅ | 1965 ✅ | 3937 ✅ | 5837 ✅ | 6178 ✅ | 64 ✅ | 1981 ✅ | 3963 ✅ | 5845 ✅ | 6201 ⚠️ |
| PC-1500 | CE-155 | 50 ✅ | 3317 ✅ | 6668 ✅ | 9934 ✅ | 10278 ✅ | 64 ✅ | 3346 ✅ | 6693 ✅ | 9941 ✅ | 10297 ⚠️ |
| PC-1500 | CE-161 | 50 ✅ | 6052 ✅ | 12154 ✅ | 18125 ✅ | 18482 ✅ | 64 ✅ | 6077 ✅ | 12155 ✅ | 18133 ✅ | 18489 ⚠️ |
| PC-1500A | none | 50 ✅ | 1965 ✅ | 3937 ✅ | 5837 ✅ | 6178 ✅ | 64 ✅ | 1981 ✅ | 3963 ✅ | 5845 ✅ | 6201 ⚠️ |
| PC-1500A | CE-151 | 50 ✅ | 3317 ✅ | 6668 ✅ | 9934 ✅ | 10278 ✅ | 64 ✅ | 3346 ✅ | 6693 ✅ | 9941 ✅ | 10297 ⚠️ |
| PC-1500A | CE-155 | 50 ✅ | 4700 ✅ | 9414 ✅ | 14036 ✅ | 14368 ✅ | 64 ✅ | 4711 ✅ | 9424 ✅ | 14037 ✅ | 14393 ⚠️ |
| PC-1500A | CE-161 | 50 ✅ | 7429 ✅ | 14861 ✅ | 22229 ✅ | 22575 ✅ | 64 ✅ | 7442 ✅ | 14885 ✅ | 22229 ✅ | 22585 ⚠️ |

**72 of 80 cells pass.** In every BASIC and machine-code load that fits,
both paths leave the program bytes, BASPRG_ST / BASPRG_END and all of user
RAM identical.

### Differences

- **Machine code, OVER tier (8 cells): the ROM has no bounds check.**
  `CLOAD M` takes the whole stream and writes past the end of RAM:
  - Into open bus, where the bytes are lost and the ROM shows no error.
  - On PC-1500A + CE-155, into the display-RAM window at &7000. There the
    loader writes the same bytes and also loads.

  Everywhere else the loader refuses with "&xxxx is not RAM". This is a
  deliberate difference: the loader refuses a file that doesn't fit.
- **BASIC capacity edge: the ROM accepts one byte more** (`edge_pc1500.py`
  on PC-1500 / none, PC-1500A / CE-161 and PC-1500A / CE-155):
  - Up to payload = free, both paths load, and the memory is identical.
  - At payload = free + 1, the ROM loads and puts the 0xFF end mark at
    BASPRG_END = RAM_END, one byte beyond RAM. The program only ends there
    because open bus reads 0xFF. With a CE-155 on a PC-1500A that byte is
    &7000, in display RAM. The loader counts the end mark and refuses.
  - At free + 2, the ROM refuses with ERROR 22 after the header, like the
    OVER tier.

  The loader's stricter limit is the safe one.

# PC-1600

## The four sets

Which transfers the ROM supports (annotated disassembly,
`Ref/PC-1600/PC-1600-Load-Save-Matrix.md`, checked against the CE-158 ROM):

| Set | What | Path on the PC-1600 | MODE |
|---|---|---|---|
| 1 | PC-1600 software via COM1: | `LOAD "COM1:"` (BASIC, PC-1600 header type 21H), `BLOAD "COM1:"` (machine code, type 10H); sde `--device pc1600` | 0 and 1 |
| 2 | PC-1500 software via COM1: | machine code only: `BLOAD "COM1:"` of LH5801 code behind a PC-1600 header at the LH5803 address + 8000H. Binary PC-1500 BASIC has no path: `K_LOAD` (rom3b 6E22H) takes binary only when byte 0 is FFH, so a CE-158 header is read as text. | 1 |
| 3 | PC-1600 software via CE-158 | BASIC only: a PC-1600 listing sent with sde `--device pc1500` (PC-1600-only keywords stay text); machine code would be set 4 again | 1 |
| 4 | PC-1500 software via CE-158 | `SETDEV CI` (without `"COM…:"` the native `SETDEV` passes it on to the CE-158), then `CLOAD` / `CLOAD M`. MODE 0 refuses `CLOAD` with ERROR 110 (`X_CHKTOK`). | 1 (refusal: 0) |

`CLOAD M` over the CE-158 works: `CLOAD_ENTRY` (CE-158 ROM &90C3) accepts
the `r`, `M` and `a` suffixes (&90D5 tests `M`). The Ref load/save matrix
says "no ML transfer on the CE-158" in MODE 1; that is wrong.

## Running it

```sh
dev/loader-matrix/run_pc1600.sh            # build, generate, run all 250 cells (about 15 minutes)
python3 dev/loader-matrix/edge_pc1600.py 0 none none com1 11834   # BASIC capacity edge
```

| File | What it does |
|---|---|
| `pc1600_loader_matrix.cpp` | The harness (`--mode --slot1 --slot2 --transport com1\|ce158 --kind --stream --loader-file [--expect-refuse]`, or `--probe`). In-process, through `applyPC1600Preset`, CE-158 always attached. |
| `build_pc1600.sh` | Builds the harness. |
| `gen_pc1600.py` | Probes each MODE and module set, writes the programs with `sde convert` and the manifest. |
| `run_pc1600.sh` | Runs the manifest; results in `headless/loader-matrix/results1600.md`, logs in `logs1600/`. |
| `edge_pc1600.py` | Byte-exact BASIC listings around the capacity limit. |

- **COM1:** `SETCOM "COM1:",9600,8,N,1,N,N` and `RCVSTAT "COM1:",28`, then
  `LOAD`/`BLOAD "COM1:"`. The peer paces like sde: the 16-byte header, 300 ms,
  then one byte per 1 ms plus a character time.
- **CE-158:** `SETCOM 1200`, `SETDEV CI`, then `CLOAD`/`CLOAD M`, as on the
  PC-1500.
- **The loader run** types the same setup lines, then loads with
  `program: file:`. It gets the `.bas` listing in MODE 0 and for the CE-158
  sets. Set 1 in MODE 1 gets the `.bbin`: a listing is tokenized with the
  PC-1500 table in MODE 1 (Decisions.md), but `LOAD` takes PC-1600 tokens.

## Pass criteria

A cell passes when both loads succeed and these are byte-identical:
- internal RAM &C000–&EFFF
- each slot card's whole backing store
- the program pointers: the slot descriptors F016–F023, F02A–F02C, F1C1,
  TITLE/ADTBL F1D5–F1DA, BASPRG_ST/END/EDT F865–F86A, VARIABLE_PTR F899,
  F89D/F89E, PRGADR FE3C–FE41

The rest of the work area is listed per run for information. It differs
only in transfer and interpreter scratch:
- IOCS / COM receive state and buffer (F05C–F17F)
- interpreter work and the default FCB (F31D–F4FF)
- the Z-80 stack (F500–F5FF; &F5CF is a stack slot, not a RAM-base pointer)
- the BASIC accumulators (FA00–FA3F)
- the header buffer (FB60–FB8F)
- the device name (FC16)

On the CE-158 path the LH5803-side copies show up as well: &F855 is the
PC-1500 CR/LF register &7855, and &FB60 is OUT_BUF &7B60.

## Matrix and results (2026-10-03)

| MODE | Modules | BASPRG_ST (LH5803) | MEM |
|---|---|---|---|
| 0 / 1 | none | &40C5 | 11834 |
| 0 / 1 | CE-151@S1 | &30C5 | 15930 |
| 0 / 1 | CE-155@S1 | &20C5 | 20026 |
| 0 / 1 | CE-161@S1 or @S2 | &00C5 | 28218 |
| 0 | CE-1600M@S1 | &00C5 | 44602 |
| 0 | CE-1600M@S1 + CE-1600M@S2 | &00C5 | 77370 |

MODE 1 allows at most 16 KB of module (`PC15MAP`), so the CE-1600M
configurations are MODE 0 only. The tiers are as on the PC-1500, taken from
MEM:
- **BASIC:** the program area.
- **Set 1 machine code:**
  - internal RAM from &C0C6
  - the first module bank from BASPRG_ST+1 to &BFFF; its OVER tier runs on
    into &C000
- **Sets 2 and 4 machine code:** LH5803 BASPRG_ST+1 up to VARIABLE_PTR,
  which crosses from the module into internal RAM.

| Set | MODE | Modules | Kind | Target | S | M | L | XL | OVER |
|---|---|---|---|---|---|---|---|---|---|
| 1 PC-1600 via COM1: | 0 | none | basic | program area | 58 ✅ | 3927 ✅ | 7884 ✅ | 11706 ✅ | 12077 ✅ |
| 1 PC-1600 via COM1: | 0 | none | ml | &C0C6 internal | 64 ✅ | 3859 ✅ | 7718 ✅ | 11478 ✅ | – |
| 1 PC-1600 via COM1: | 0 | CE-151@S1 | basic | program area | 58 ✅ | 5284 ✅ | 10600 ✅ | 15808 ✅ | 16175 ✅ |
| 1 PC-1600 via COM1: | 0 | CE-151@S1 | ml | &C0C6 internal | 64 ✅ | 3859 ✅ | 7718 ✅ | 11478 ✅ | – |
| 1 PC-1600 via COM1: | 0 | CE-151@S1 | ml | module bank 0 | 64 ✅ | 1299 ✅ | 2598 ✅ | 3798 ✅ | 4154 ✅ |
| 1 PC-1600 via COM1: | 0 | CE-155@S1 | basic | program area | 58 ✅ | 6656 ✅ | 13342 ✅ | 19917 ✅ | 20260 ✅ |
| 1 PC-1600 via COM1: | 0 | CE-155@S1 | ml | &C0C6 internal | 64 ✅ | 3859 ✅ | 7718 ✅ | 11478 ✅ | – |
| 1 PC-1600 via COM1: | 0 | CE-155@S1 | ml | module bank 0 | 64 ✅ | 2664 ✅ | 5329 ✅ | 7894 ✅ | 8250 ✅ |
| 1 PC-1600 via COM1: | 0 | CE-161@S1 | basic | program area | 58 ✅ | 9379 ✅ | 18809 ✅ | 28099 ✅ | 28453 ✅ |
| 1 PC-1600 via COM1: | 0 | CE-161@S1 | ml | &C0C6 internal | 64 ✅ | 3859 ✅ | 7718 ✅ | 11478 ✅ | – |
| 1 PC-1600 via COM1: | 0 | CE-161@S1 | ml | module bank 0 | 64 ✅ | 5395 ✅ | 10790 ✅ | 16086 ✅ | 16442 ✅ |
| 1 PC-1600 via COM1: | 0 | CE-161@S2 | basic | program area | 58 ✅ | 9379 ✅ | 18809 ✅ | 28099 ✅ | 28453 ✅ |
| 1 PC-1600 via COM1: | 0 | CE-161@S2 | ml | &C0C6 internal | 64 ✅ | 3859 ✅ | 7718 ✅ | 11478 ✅ | – |
| 1 PC-1600 via COM1: | 0 | CE-161@S2 | ml | module bank 2 | 64 ✅ | 5395 ✅ | 10790 ✅ | 16086 ✅ | 16442 ✅ |
| 1 PC-1600 via COM1: | 0 | CE-1600M@S1 | basic | program area | 58 ✅ | 14855 ✅ | 29710 ✅ | 44489 ✅ | 44838 ✅ |
| 1 PC-1600 via COM1: | 0 | CE-1600M@S1 | ml | &C0C6 internal | 64 ✅ | 3859 ✅ | 7718 ✅ | 11478 ✅ | – |
| 1 PC-1600 via COM1: | 0 | CE-1600M@S1 | ml | module bank 0 | 64 ✅ | 5395 ✅ | 10790 ✅ | 16086 ✅ | 16442 ✅ |
| 1 PC-1600 via COM1: | 0 | CE-1600M@S1 + CE-1600M@S2 | basic | program area | 58 ✅ | 25765 ✅ | 51575 ✅ | 77264 ✅ | 77616 ✅ |
| 1 PC-1600 via COM1: | 0 | CE-1600M@S1 + CE-1600M@S2 | ml | &C0C6 internal | 64 ✅ | 3859 ✅ | 7718 ✅ | 11478 ✅ | – |
| 1 PC-1600 via COM1: | 0 | CE-1600M@S1 + CE-1600M@S2 | ml | module bank 2 | 64 ✅ | 5395 ✅ | 10790 ✅ | 16086 ✅ | 16442 ✅ |
| 1 PC-1600 via COM1: | 1 | none | basic | program area | 58 ✅ | 3927 ✅ | 7884 ✅ | 11706 ✅ | 12077 ✅ |
| 1 PC-1600 via COM1: | 1 | none | ml | &C0C6 internal | 64 ✅ | 3859 ✅ | 7718 ✅ | 11478 ✅ | – |
| 2 PC-1500 ML via COM1: | 1 | none | ml | LH BASPRG_ST+1 | 64 ✅ | 3859 ✅ | 7718 ✅ | 11478 ✅ | – |
| 2 PC-1500 ML via COM1: | 1 | none | ml | same, loader gets CE-158 file | 64 ✅ | 3859 ✅ | 7718 ✅ | 11478 ✅ | – |
| 4 PC-1500 via CE-158 | 1 | none | ml | LH BASPRG_ST+1 | 64 ✅ | 3859 ✅ | 7718 ✅ | 11478 ✅ | – |
| 3 PC-1600 BASIC via CE-158 | 1 | none | basic | program area | 42 ✅ | 3925 ✅ | 7864 ✅ | 11713 ✅ | 12078 ✅ |
| 4 PC-1500 via CE-158 | 1 | none | basic | program area | 50 ✅ | 3937 ✅ | 7871 ✅ | 11720 ✅ | 12070 ✅ |
| 1 PC-1600 via COM1: | 1 | CE-151@S1 | basic | program area | 58 ✅ | 5284 ✅ | 10600 ✅ | 15808 ✅ | 16175 ✅ |
| 1 PC-1600 via COM1: | 1 | CE-151@S1 | ml | &C0C6 internal | 64 ✅ | 3859 ✅ | 7718 ✅ | 11478 ✅ | – |
| 2 PC-1500 ML via COM1: | 1 | CE-151@S1 | ml | LH BASPRG_ST+1 | 64 ✅ | 5224 ✅ | 10449 ✅ | 15574 ✅ | – |
| 2 PC-1500 ML via COM1: | 1 | CE-151@S1 | ml | same, loader gets CE-158 file | 64 ✅ | 5224 ✅ | 10449 ✅ | 15574 ✅ | – |
| 4 PC-1500 via CE-158 | 1 | CE-151@S1 | ml | LH BASPRG_ST+1 | 64 ✅ | 5224 ✅ | 10449 ✅ | 15574 ✅ | – |
| 3 PC-1600 BASIC via CE-158 | 1 | CE-151@S1 | basic | program area | 42 ✅ | 5290 ✅ | 10600 ✅ | 15814 ✅ | 16160 ✅ |
| 4 PC-1500 via CE-158 | 1 | CE-151@S1 | basic | program area | 50 ✅ | 5286 ✅ | 10611 ✅ | 15813 ✅ | 16160 ✅ |
| 1 PC-1600 via COM1: | 1 | CE-155@S1 | basic | program area | 58 ✅ | 6656 ✅ | 13342 ✅ | 19917 ✅ | 20260 ✅ |
| 1 PC-1600 via COM1: | 1 | CE-155@S1 | ml | &C0C6 internal | 64 ✅ | 3859 ✅ | 7718 ✅ | 11478 ✅ | – |
| 2 PC-1500 ML via COM1: | 1 | CE-155@S1 | ml | LH BASPRG_ST+1 | 64 ✅ | 6590 ✅ | 13180 ✅ | 19670 ✅ | – |
| 2 PC-1500 ML via COM1: | 1 | CE-155@S1 | ml | same, loader gets CE-158 file | 64 ✅ | 6590 ✅ | 13180 ✅ | 19670 ✅ | – |
| 4 PC-1500 via CE-158 | 1 | CE-155@S1 | ml | LH BASPRG_ST+1 | 64 ✅ | 6590 ✅ | 13180 ✅ | 19670 ✅ | – |
| 3 PC-1600 BASIC via CE-158 | 1 | CE-155@S1 | basic | program area | 42 ✅ | 6666 ✅ | 13339 ✅ | 19926 ✅ | 20278 ✅ |
| 4 PC-1500 via CE-158 | 1 | CE-155@S1 | basic | program area | 50 ✅ | 6668 ✅ | 13330 ✅ | 19899 ✅ | 20273 ✅ |
| 1 PC-1600 via COM1: | 1 | CE-161@S1 | basic | program area | 58 ✅ | 9379 ✅ | 18809 ✅ | 28099 ✅ | 28453 ✅ |
| 1 PC-1600 via COM1: | 1 | CE-161@S1 | ml | &C0C6 internal | 64 ✅ | 3859 ✅ | 7718 ✅ | 11478 ✅ | – |
| 2 PC-1500 ML via COM1: | 1 | CE-161@S1 | ml | LH BASPRG_ST+1 | 64 ✅ | 9320 ✅ | 18641 ✅ | 27862 ✅ | – |
| 2 PC-1500 ML via COM1: | 1 | CE-161@S1 | ml | same, loader gets CE-158 file | 64 ✅ | 9320 ✅ | 18641 ✅ | 27862 ✅ | – |
| 4 PC-1500 via CE-158 | 1 | CE-161@S1 | ml | LH BASPRG_ST+1 | 64 ✅ | 9320 ✅ | 18641 ✅ | 27862 ✅ | – |
| 3 PC-1600 BASIC via CE-158 | 1 | CE-161@S1 | basic | program area | 42 ✅ | 9405 ✅ | 18806 ✅ | 28102 ✅ | 28457 ✅ |
| 4 PC-1500 via CE-158 | 1 | CE-161@S1 | basic | program area | 50 ✅ | 9381 ✅ | 18790 ✅ | 28113 ✅ | 28448 ✅ |
| 1 PC-1600 via COM1: | 1 | CE-161@S2 | basic | program area | 58 ✅ | 9379 ✅ | 18809 ✅ | 28099 ✅ | 28453 ✅ |
| 1 PC-1600 via COM1: | 1 | CE-161@S2 | ml | &C0C6 internal | 64 ✅ | 3859 ✅ | 7718 ✅ | 11478 ✅ | – |
| 2 PC-1500 ML via COM1: | 1 | CE-161@S2 | ml | LH BASPRG_ST+1 | 64 ✅ | 9320 ✅ | 18641 ✅ | 27862 ✅ | – |
| 2 PC-1500 ML via COM1: | 1 | CE-161@S2 | ml | same, loader gets CE-158 file | 64 ✅ | 9320 ✅ | 18641 ✅ | 27862 ✅ | – |
| 4 PC-1500 via CE-158 | 1 | CE-161@S2 | ml | LH BASPRG_ST+1 | 64 ✅ | 9320 ✅ | 18641 ✅ | 27862 ✅ | – |
| 3 PC-1600 BASIC via CE-158 | 1 | CE-161@S2 | basic | program area | 42 ✅ | 9405 ✅ | 18806 ✅ | 28102 ✅ | 28457 ✅ |
| 4 PC-1500 via CE-158 | 1 | CE-161@S2 | basic | program area | 50 ✅ | 9381 ✅ | 18790 ✅ | 28113 ✅ | 28448 ✅ |
| 4 PC-1500 via CE-158 | 0 | none | basic | program area | refused ✅ | – | – | – | – |
| 4 PC-1500 via CE-158 | 0 | none | ml | LH BASPRG_ST+1 | refused ✅ | – | – | – | – |

**250 of 250 cells pass** after two loader fixes (commit 50dc20c). The first
run (205 cells, before CE-151 was added) found 50 failures:
- **Wrong place in a CE-155** (26 cells). The loaders wrote a module's bytes
  at "address minus window base" into the card image. A CE-155's image isn't
  in address order: A000H (S1) is at 0800H, and B800H (its own decoder) is
  at 0000H. So a BASIC program landed in the wrong chip, and machine code
  was refused. The loaders now write through the slot's pins in the
  segment's bank (`PC1600Memory::slotBusWrite`).
- **Machine code crossing &C000** (23 cells). `BLOAD` and `CLOAD M` write
  linearly from a module window on into internal RAM, but the loader
  refused anything crossing &C000. In MODE 1 the module and internal RAM
  are one LH5803 range, so ordinary PC-1500 code bigger than the module
  part was refused. It now continues into internal RAM.

One more failure was a harness mistake: the MODE 0 CE-158 refusal cell first
gave the loader a `.bas`, which MODE 0 loads legitimately.

### Capacity edge (`edge_pc1600.py`)

- **COM1:, MODE 0, no module:** ROM and loader agree to the byte. Both
  load up to MEM − 2 and refuse from MEM − 1.
- **COM1:, MODE 0, CE-1600M in both slots:** the limit is below MEM,
  because no line may straddle a module bank and each bank ends with a
  00 00 mark. ROM and loader agree to the line: both take 77 322 bytes
  (MEM 77 370) and both refuse the next line, with identical memory.
- **CE-158, MODE 1, CE-161:** the ROM's `CLOAD` takes up to MEM + 1. As on
  the PC-1500, the end mark then lands one byte past the user area. The
  loader stops at MEM − 2, the native `LOAD` limit. It therefore refuses
  MEM − 1 and MEM, which `CLOAD` loads correctly. Open: whether a CE-158
  file in MODE 1 should get `CLOAD`'s limit (MEM).
