# Loader matrix: CE-158/sde vs. fast loader (PC-1500/1500A)

**Status: implemented 2026-10-03.** Results in `dev/loader-matrix/README.md`. How it differed:
- The CE-151 has no PC-1600 host. Its S1/S2 land at A000–AFFF in Slot 1, which leaves a hole below internal RAM.
- The sde PTY capture (step 3) is impossible for a PC-1500 device. The check was done in sde's source and dry run instead.
- The ML OVER tier showed that `CLOAD M` has no bounds check.
- An extra capacity-edge scan (`edge_pc1500.py`) was added.


## Context
The fast BASIC loader and the machine-code loader write memory directly,
without the ROM's help. They have never been checked against a real ROM load
across all memory configurations. For every combination of model, memory card,
program kind and size, we load the same program twice: once through the ROM
(`CLOAD` / `CLOAD M` over the CE-158, fed the bytes sde sends) and once through
our loader. Then we compare the memory. The CE-158 stays attached in both runs.
CE-151 and CE-161 don't exist yet as cards, so step 1 adds them.

## Step 1: CE-151 and CE-161 cards
New `Qt6/resources/cards/ce151.card.yaml` and `ce161.card.yaml`, modelled on
`ce155.card.yaml`, with PC-1500 terminology and plain Regular RAM
(`power-up-fill: 0x00`, no battery, no write-protect, as you chose).
- **CE-151, 4 KB:** two 2 KB chips (HM6116/TC5517), wired per the Service
  Manual schematic (5-3): chip 1 CS = `S1`, chip 2 CS = `S2`, A0–A10 = offset.
  There's no Y0 path. `any-of`:
  - `S1` (span 0x800 → 0x0000)
  - `S2` (span 0x800 → 0x0800)

  That gives &4800–&57FF on the PC-1500, and &5800–&67FF on the PC-1500A
  (pins 16/17 = S3/S4 there). Both sit right above built-in RAM, so
  BASPRG_ST stays &40C5. The header comment cites the Service Manual
  schematic.
  - The spec §9 row ("(Y0 AND AD11–13) OR S1") is corrected to `S1 OR S2`.
  - The Ref repo's Software-Defined-Memory-Extension.md row (marked
    "inferred") is wrong the same way. I won't edit it; I'll point it out
    to you.
- **CE-161, 16 KB:** `Y0` alone, span 0x4000 → 0x0000 (&0000–&3FFF).
- **Hosts:** CE-151 `[PC-1500, PC-1500A, PC-1600-Slot-1]`, CE-161
  `[PC-1500, PC-1500A, PC-1600-Slot-1, PC-1600-Slot-2]`. PC-1600 expectation
  (`Ref/PC-1600/PC-1600-Memory-Architecture.md`): `MEM` = 11 834 + 4 096 / + 16 384.
  The CE-151's S1/S2 wiring has to show up on PC-1600 Slot 1 as B000–BFFF.
  If the Slot-1 test doesn't get +4 096 that way, I'll take PC-1600 off the
  CE-151's host list and report it rather than change the pin routing.
- **Tests** (new `Core/tests/ce151_tests.cpp`, `ce161_tests.cpp`, same style as
  `ce155_tests.cpp`, registered in `CMakeLists.txt`, `tools/run_tests.sh` and
  `lh5801_tests.cpp`'s runner list):
  - pin windows: no aliasing outside them
  - end-to-end on PC-1500 and PC-1500A: RAM_ST/RAM_END and BASPRG_ST after
    NEW0 (CE-151 → &40C5, RAM_END raised by 4 K; CE-161 → &00C5)
  - These card tests do go into the standard suite, since they test a
    shipped feature. Only the loader matrix below stays out of it.
  - PC-1600 Slot 1 (and Slot 2 for CE-161), in `pc1600_slot_module_tests.cpp`
    next to the CE-155 one
- **Docs:**
  - User-Guide module table (line ~307)
  - Format.md §7 file table
  - Spec §9 hosts column for both rows (now including the PC-1600 slots)
  - CHANGELOG
- Commit.

## Where the matrix lives
Everything for the matrix goes in its own directory, **`dev/loader-matrix/`**,
with one row added to `dev/README.md`. It is a one-time verification:
- it is not in `CMakeLists.txt`, `tools/run_tests.sh` or the CoreTests runner
- it has its own build script
- binaries and generated programs go to `headless/loader-matrix/`

Contents:
- `README.md`: purpose, how to run it, the matrix and the results
- `pc1500_loader_matrix.cpp`, `build.sh`
- `gen_pc1500.py`, `run_pc1500.sh`

## Step 2: harness
`dev/loader-matrix/pc1500_loader_matrix.cpp`, built by
`dev/loader-matrix/build.sh` into `headless/loader-matrix/`. It runs everything in-process
and reuses `applyPC1500Preset` (`Core/PC1500/PC1500PresetLoader.hpp`), so
both paths go through the real preset/card/CE-158 attach code. It is
deterministic, with no PTY.

Each run builds a preset text in memory: `model:`, `interface: CE-158`,
optional `slot-1: <card>`, `keys: [cl, type NEW0]`. Then:

- **Path A (ROM over CE-158).** The preset also types `SETCOM 1200`,
  `SETDEV CI`, then `CLOAD` (BASIC) or `CLOAD M` (ML).
  - A harness `SerialLink` (like `FakeLink` in `ce158_tests.cpp`) is armed
    once the preset returns. It feeds the sde stream: the 27-byte header,
    then a 300 ms emulated pause (sde's `HEADER_PAUSE`), then the payload.
  - Run until the stream is consumed, plus about 2 s emulated time.
  - Read `pc1500LcdText`: no `ERROR`, prompt back.
- **Path B (our loader).** Same preset, plus `program: file: <file>`, so the
  real loader dispatch runs (`ProgramFile` → `loadBasicProgramFile` /
  `loadPC1500MachineCode`).
  - The BASIC input is the `.bas` listing, so tokenization is cross-checked
    as well.
  - The ML input is the sde `.ce158.bin` (header gives the address).
- **Compare.** Snapshot ME0 &0000–&7FFF via `memory().peek` after each path.
  - **PASS when:** both loads succeed; the loaded range (BASPRG_ST..BASPRG_END
    for BASIC, the header range for ML) matches the expected payload in both;
    and all of user RAM [RAM_ST·256, RAM_END·256) plus the pointer block
    &7860–&786F are byte-identical.
  - Differences in the rest of system RAM (&7600–&7FFF: input buffer,
    display, stack, CE-158 work area) are listed as address ranges, triaged
    once, and the expected ones go on an allowlist with a reason.
- **`--probe` mode** prints BASPRG_ST, RAM_ST/RAM_END and free bytes for a
  model+card, to size the programs.
- **`--stream <file>`** overrides the serial bytes (used by step 3).

## Step 3: check that the sde stream matches the wire
A one-off script in the scratchpad:
- open a PTY pair (python stdlib `os.openpty`)
- run `sde put <file> --device pc1500 --port <slave>`
- record what arrives on the master

It confirms that the wire bytes equal the `sde convert` output (header +
payload, header_len 27) for one BASIC and one ML file. If they differ, the
harness feeds the captured stream (`--stream`) instead.

## Step 4: programs and matrix
`dev/loader-matrix/gen_pc1500.py` (stdlib only, run with `python3`) writes
into `headless/loader-matrix/`:
- **BASIC:** a deterministic generated listing with a mix of statements
  (PRINT strings, FOR/NEXT, arithmetic, DATA, string functions, REM). Lines
  are added until `sde convert`'s `.bbin` reaches the target size.
- **ML:** deterministic LFSR bytes (any misplacement shows), turned into
  `.ce158.bin` by `sde convert --start-address <BASPRG_ST+1>`. That keeps
  NEW0's 0xFF end mark. There is no auto-run (run = FFFF).

Matrix, 2 models × 4 cards × 2 kinds × 5 sizes = 80 cells:

| Model | Card | Program area (expected) | ~free |
|---|---|---|---|
| PC-1500 | none | &40C5–&47FF | 1.8 K |
| PC-1500 | CE-151 | &40C5–&57FF | 5.9 K |
| PC-1500 | CE-155 | &38C5–&5FFF | 10 K |
| PC-1500 | CE-161 | &00C5–&47FF | 18 K |
| PC-1500A | none | &40C5–&57FF | 5.9 K |
| PC-1500A | CE-151 | &40C5–&67FF | 9.9 K |
| PC-1500A | CE-155 | &38C5–&6FFF | 14 K |
| PC-1500A | CE-161 | &00C5–&57FF | 22 K |

Sizes per cell (from `--probe`'s free bytes):
- **S** ≈ 64 B
- **M** ≈ ⅓ free
- **L** ≈ ⅔ free
- **XL** = free − ~100 B (almost full)
- **OVER** = free + 256 B: both paths must refuse; this tier compares
  refusal, not memory.

A larger card's program crosses its windows (CE-155: Y0 slot → built-in S0
→ S-blocks; CE-151: built-in → S-blocks; CE-161: card → built-in), so each
one tests a boundary.

`dev/loader-matrix/run_pc1500.sh` loops over the matrix. It prints one row
per cell (PASS/FAIL, diff summary) and writes the results table into the
directory's README.

## Step 5: run, triage, report
- Run the matrix.
- For every FAIL, find whether the ROM or our loader differs. Fix the loader
  in Core, never by adjusting the test (per Decisions.md).
- Commit `dev/loader-matrix/` with its results to dev-0.7.0, plus the plan
  as `docs/background/plans/Loader-Matrix-Plan.md` (+ a row in
  `docs/background/README.md`).
- Loader fixes go in separate commits.

## Verification
- `tools/run_tests.sh`: green, including the new CE-151/CE-161 card tests
  (and with no loader-matrix code in it).
- `--probe` per config matches the program-area table above. MEM on the
  PC-1600 matches the Ref figures for CE-151/CE-161.
- Step 3's PTY capture matches the `sde convert` bytes.
- All 80 cells PASS, or every remaining FAIL is explained and listed in the
  results doc.
