# LCD text parser ("screen text") — Plan

Status: implemented 2026-10-03 (8554681, cbe14a0, 6985e9c, docs). How it
works now: [../../developer/LCD-Text.md](../../developer/LCD-Text.md).
Changes from the plan below, found during implementation:
- The PC-1600 font address differs between the ROM versions; it is located
  through the 8030H `CGSET80` entry instead of fixed addresses.
- The ROMs draw the cursor *over* the cell (no glyph + overlay). The
  PC-1600 underline cursor is the `_` glyph, so the cursor is checked at
  `CRSRY`/`CRSRX` only (`LcdCursor`).
- The PC-1500's characters from 80H come from `KATACHAR`, outside the ROM:
  only 20H-7FH are parsed.
- The DE/G/RAD status segments are reported as one DEG/GRAD/RAD legend.

## Context

Checking a result on the emulated LCD today means rendering a PNG (`--lcd-png` or `- screenshot:`) and reading the image. That is slow, costs tokens and can misread characters (0/O, 1/l, a dropped minus sign). The MEM investigation needed three images to read three numbers. The preset log's `screen="…"` looks like it should help, but it is the BASIC **input buffer** (`presetInputLine` at FBB0H / 7BB0H), not the display, so it never shows results.

Both LCDs are fully known: the pixels come from the emulator, the font comes from the machine's own ROM, and text mode uses a fixed grid of 26 columns × 6 px. A cell is matched by exact lookup, so this isn't OCR. Cells that don't match (graphics, user CGs, text placed off the grid) come out as a marker and are counted, so a caller knows when to fall back to the PNG.

## Design

### Core parser — `Core/Display/LcdText.{hpp,cpp}` (model-free, like `LcdScreenshot`)

- Input: the existing `LcdBitmap` (`Core/Display/LcdScreenshot.hpp`) plus an `LcdFont`. A font has a cell width and height, a glyph width, and, per code 00H–FFH, an optional column-wise glyph (one byte per column, bit 0 = top row).
- `LcdText parseLcdText(const LcdBitmap&, const LcdFont&)` returns:
  - `rows`: one UTF-8 string per text row (PC-1500: 1 row of 7 px; PC-1600: 4 rows of 8 px);
  - `unparsed`: the number of cells that matched nothing;
  - `reverse`: cells that matched an inverted glyph;
  - `cursor`: the position of a cell matched as glyph + cursor overlay;
  - `poweredOn`.
- Matching per cell: an all-blank cell is a space; otherwise exact glyph, then inverted glyph, then glyph with the cursor overlay. When several codes share a glyph (e.g. the PC-1600 `CGSPEC` substitutions), the lowest printable ASCII code wins.
- Output encoding: printable ASCII as-is; other ROM codes (katakana, graphics characters) as `\xHH`; unmatched cells as `�` (U+FFFD), which can't be confused with a real `?`. `toString()` joins the rows with `\n`.
- Status symbols are not in the bitmap. Each model adapter appends them as a separate `status` list, using the existing accessors: `PC1500Display::run()/pro()/shift()…` and `PC1600StatusLine::isOn(Symbol)`.

### Fonts read from the loaded ROM (original sources, no embedded tables)

- **PC-1500** — `Core/PC1500/PC1500LcdText.hpp`: `CHARSET` at FCA0H, 5 bytes per character for 20H–7FH (`PC-1500_ROM-A0x.lh5801.asm` ~14197), and the second character set that `CHAR_2_ADDR` ($EE48) picks for codes ≥ 80H. Bytes come from `PC1500Memory` (ROM `peek`), so A01/A03/A04 each use their own font. The cell is 6 × 7.
- **PC-1600** — `Core/PC1600/PC1600LcdText.hpp`: bank 6 `CGLOW` ADA4H (20H–7FH) and `CGHIGH` AFE4H (80H–FFH), 6 bytes column-wise, 6 × 8 cell (`notes/PC1600-P2-B6-Disassembly.md`). This needs a const `bank6Rom()` accessor on `PC1600Memory` (`m_bank6Rom`, `PC1600Memory.hpp:572`).
- Cursor overlay: take its shape from the display IOCS (bank 6 cursor routines, `CRSRSAV` F069H) or the PC-1500 cursor routine. Look up the exact shape during implementation and cite the ROM address in a comment.
- User CGs (`UPACGA`/`CGSET80`) are left out of v1. Redefined characters come out as `�`.

### Wiring

1. **Preset log:** rename the existing `screen=` to `input=`, and add `lcd="row1|row2|…"` from the parser. `PresetMachine::stepTag()` stays; both adapters build the new tag. Pre-1.0, so no compatibility shim.
2. **CLIs:** `--lcd-text <file|->` in `tools/pc1500_cli.cpp` and `tools/pc1600_cli.cpp`, next to `--lcd-png`, written at the end of the run. Adds a `status:` line and, when nonzero, an `unparsed: n` line.
3. **Preset step `- expect: <text>`:** passes if any LCD row contains `<text>`; otherwise the preset fails and logs the full screen. Add it to `Core/Preset/PresetFile.{hpp,cpp}` (verb parsing next to `screenshot`, ~line 255) and `PresetRunner.cpp`, with a new `PresetMachine::screenText()` virtual. This lets the agent and the tests check results without a probe.
4. **DAP:** add a `calcu1600/screen` custom request in `Qt6/app/debug/DapSession.cpp` (next to `calcu1600/load`/`reset`). It returns `{rows, status, unparsed, cursor}`, which reads the running GUI app without a screenshot.

## Phases (commit each to dev-0.7.0)

1. Core parser, both font builders, `bank6Rom()`, and unit tests.
2. Preset log `input=`/`lcd=`, the `expect:` step, and the CLI `--lcd-text`.
3. The DAP `calcu1600/screen` request, plus a check in `tools/dap_smoke.py`.
4. Docs:
   - `expect:` and `--lcd-text` in the preset/CLI docs;
   - a short developer note on the parser (fonts, encoding, limits) under `docs/developer/` and its index;
   - a memory entry telling the agent to use `--lcd-text`/`expect:` before images.

## Verification

- **Unit tests (Core/tests):**
  - Round trip on both machines: `FOR I=32 TO 255: CLS: PRINT CHR$(I)…` checking every code (or a few PRINT lines of all codes), parsed back with `unparsed == 0`.
  - A synthetic bitmap test for reverse, cursor overlay and unknown cells.
  - A `GCIRCLE`/`LINE` screen reports `unparsed > 0`.
- **The MEM case from this session:** a preset with `PRINT MEM` and `- expect: 11834` (plain) / `10810` (CE-1600P) passes, and `--lcd-text -` prints the same numbers the PNGs showed.
- **PC-1500:** a `PRINT` result and a `RUN`/`PRO` status line come out right on A04 and A03.
- Full test suite green; `tools/dap_smoke.py` against the GUI app, launched with `-ApplePersistenceIgnoreState YES` and quit via `calcu1600/quit`.
