# Loader matrix

A one-time verification of the fast loaders against the ROM. Each program
is loaded into a PC-1500/1500A twice, and the memory is compared afterwards:

- **ROM:** `SETCOM 1200`, `SETDEV CI`, then `CLOAD` (BASIC) or `CLOAD M`
  (machine code) over the CE-158. The serial peer sends what `sde put`
  sends: the 27-byte CE-158 header, a 300 ms pause, then the payload.
- **Loader:** the same machine with `SETCOM`/`SETDEV` typed, then a preset
  `program: file:`. BASIC gets the `.bas` listing, so the in-process
  tokenizer is checked as well. Machine code gets the `.ce158.bin`.

The CE-158 is attached in both runs. This directory is not part of
`tools/run_tests.sh` or CoreTests.

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
