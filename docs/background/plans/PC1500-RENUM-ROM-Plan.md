# PC-1500 ROM extension demo: a `RENUM` keyword

> **Status (2026-10-02): implemented.** Where the work differed from this
> plan:
> - **A missing target aborts.** The plan said "renumber anyway, report".
>   The PC-1600 ROM actually skips RENNUMBER when RENSCAN finds a missing
>   line, so RENUM reports `ERROR 11 IN n` and leaves the program unchanged.
>   A computed reference is `ERROR 1 IN n`. Both are shown as an error in
>   that line, by setting 789CH/789EH before `VEJ E0`.
> - **The keyword's attributes and the mode check** were found in the ROM.
>   The marker after an entry carries its run attributes (`CNIB`). RENUM is
>   "typed only", with a mode-mask byte (`60H`: RUN and PRO) in front of
>   its code, like LIST/RUN/NEW. Inside a program the ROM itself raises
>   ERROR 1.
> - **The code's high byte has a plain rule** (`TOK_PROCESS` FAA8): page
>   `80H + (code&7)×8`, PV high for E8H–EFH. So `E180` is &8800 with PV low.
> - **The work variables live in the string buffer** (7B10H). They are
>   written only after the arguments are evaluated.
> - **A merged program is refused** (ERROR 1).
> - **The keyword-module description is user-level**:
>   `docs/PC1500-Keyword-Modules.md`, not `docs/background/`.
> - **sdaslh5801 writes `vej 0xDE`**, not `vej (DE)`.
> - **No Core changes.** The VS Code walkthrough (verification step 5) is
>   still a user check.

## Context

The debugger's *PC-1500 ROM extension* template (`vscode/calcu1600-debug/templates/pc1500-bus-rom/`) is only `CALL &8000` into a counter. It shows how to plug a ROM in, but not what a real extension does. We replace it with a working one: a bus ROM that adds a BASIC statement, `RENUM`, using the firmware's own keyword-module mechanism (sentinel, first-letter index, keyword table, boot init entry). That gives a concrete thing to build, plug in, set breakpoints in and step through.

Decisions (user):
- **Replace the template.** *Create Debug Project… → PC-1500 ROM extension* now generates the RENUM project.
- **Syntax:** the PC-1600's `RENUM [new][,[old][,step]]`. Defaults are 10, the first line, 10. Lines below `old` keep their numbers.
- **Bad references, as on the PC-1600:**
  - A reference that can't be converted (`GOTO 100+X`) is an error, and the program stays unchanged.
  - A reference to a missing line is left as it is. RENUM still renumbers, then reports `Undefined in nnn`, or the closest the PC-1500 can show.

## Key facts the design rests on

- **The algorithm comes from the PC-1600 ROM, its structure only.**
  - The code is `K_RENUM` / `RENSCAN` / `RENNUMBER` / `RENFIX` in `~/Development/sharp/pc1600/disasm/rom/pc1600/new/PC1600-P1-B3B.asm` (45BDH–48BDH).
  - The flow: parse and check arguments, then three passes (collect, renumber, fix references). It ends with a run-state reset (`RUNSET4`).
- **The PC-1500 stores line references differently.**
  - The PC-1600 turns a reference into a binary `1FH hi lo` record and patches it in place.
  - The PC-1500 keeps `GOTO 100` as ASCII digits after the token: `BCMD_GOTO` $C515 evaluates an expression, and `FIND_LINE` $CC86 looks it up.
  - So a rewritten reference can change a line's length: the line's length byte changes, the rest of the program moves, and `BASPRG_END` ($7867) moves with it.
  - That's the one real algorithmic difference from the PC-1600.
- **The keyword-module mechanism** comes from Paul Chambre's analysis (`~/Downloads/PC1500_BASIC_Keyword_Extension_Mechanism.md`). Its results were found on pc1500emu, so every point we rely on gets checked against Jeff Birt's A0x disassembly (`~/Development/sharp/Sharp_PC-1500_ROM_Disassembly/`) and against Calc-U-1600 before we cite it. Docs cite the ROM addresses, not pc1500emu. The points:
  - page layout: `55H` at +0, init entry at +0AH (the boot scan `E4A8`/`E107` calls it, so it must be a real `RTN`), the A–Z index at +20H, the table at +54H;
  - the entry format: marker (low nibble = length), the name, the code (big-endian), the address;
  - the code high byte for a PV-low page is `E0|page index`, and the low byte is `80H` or more for a statement;
  - the routine is entered with `Y` just past the token, parses its own arguments (`VEJ C0/C2/C8/DE/D0`), and ends with `VEJ E2`;
  - errors are raised with `VEJ E0` / `UH`.
- **Placement:** page `&8800`, `pv: 0`, code `E180`.
  - It avoids `&8000`/PV-low and its `E0` skip-scan quirk.
  - It coexists with the CE-150 (A000–BFFF PV-low) and the CE-158 (8000–9FFF PV-high).
  - One 2 KB page is enough for the table and the code.
- **The infrastructure already exists, so no Core changes are expected:**
  - `bus-rom:` with `address:`/`pv:` (`Core/Preset/PresetFile.cpp`, `Core/Connector/BusRomCard.hpp`);
  - the sdas build, `.rst` listings and `debug.command:` in the VS Code extension (`vscode/calcu1600-debug/extension.js`, `scaffold.js`).

## Implementation

### Step 1: Verify the mechanism in the ROM (read only)
Read the following in `PC-1500_ROM-A0x.lh5801.asm` (with the A04 `.lst` for addresses) and the CE-150 header (`CE-150.lib`, `$P15ROM/dumps/CE-150.BIN` via the lib names). Confirm the offsets and conventions above. Write down anything that differs.
- the keyword lookup / peripheral scan (`FA40`–`FB40`, the `VMJ FF3C` path), the boot module scan (`E107`, `E4A8`), and the run-time dispatch (`F0xx`/`E1xx`);
- the program-memory pointers (`BASPRG_ST/END/EDT`, the variable area / top of RAM; see `$REF/PC-1500/Memory-Architecture/PC-1500-BASIC-Pointers.md`), to work out free memory;
- a ROM routine for "clear variables / reset run state" (the PC-1600's `RUNSET4` counterpart), and one for printing a number or message, if there is one;
- which tokens take line numbers: `GOTO`, `GOSUB`, `THEN`, `ON…GOTO/GOSUB` lists, `RESTORE`, `RUN`, `LIST`/`LLIST`, `ARUN`?

Also confirm with a quick headless run that `GOTO 100` really is stored as ASCII: `headless/pc1500_cli --dump-basic`.

### Step 2: The ROM source (the new template `NAME.asm`, sdas dialect)
The layout at `.org 0x8800`:
- `55H` sentinel; `INIT` at +0AH, a plain `rtn`; the A–Z index with only `R` set (pointing at the `E` of `RENUM`); the entry `C5 'RENUM' E1 80 <RENUM>`; the terminator `D0`.
- `RENUM`, following the PC-1600's structure, with the work variables in a small RAM scratch area (e.g. the ML area `&7C01`…, documented in the source):
  1. **Guard:** refuse inside a running program (`VEJ D8`). An empty program is a no-op.
  2. **Parse** `[new][,[old][,step]]` with `VEJ C8/C2/DE/D0`, the same range checks as `GETLINENUM2`/`K_RENUM` (1..65279), with the defaults.
     - `old` must exist, as `LINSRH` requires.
     - The new numbers must not reach the line before `old`.
     - The last new number must not overflow 65279.
  3. **Dry run (RENSCAN):** walk every line and every statement, skipping quoted strings.
     - After a line-number token, take each literal reference: `,`-lists for `ON`, `THEN` + a number.
     - A reference that's an expression rather than a plain number makes RENUM fail with an error, with nothing changed.
     - Add up the length change and check it against free memory. Lines must stay ≤ 255 bytes.
     - Mark any missing target.
  4. **Fix references (RENFIX):** for each reference whose target exists:
     - work out the new number from the target's position: `new + k·step` if the target is at `old` or later, otherwise unchanged;
     - write the decimal digits, moving the rest of the program with a memmove up or down;
     - adjust the line's length byte and `BASPRG_END`.
     - This runs *before* the headers change, so lookups still see the old numbers and no table is needed. The O(n²) lookup is fine for a demo.
  5. **Renumber the headers (RENNUMBER):** rewrite the 2-byte line numbers from `old` onwards.
  6. **Reset the run state** (the ROM routine from step 1), report any missing reference, then `VEJ E2`.
- The comments say where each piece comes from (PC-1600 address, PC-1500 ROM address) so the listing teaches. Labels mirror the PC-1600 names (`RENSCAN`, `RENFIX`, `RENNUMBER`).

### Step 3: The template's preset and the scaffold
- **`templates/pc1500-bus-rom/debug.pc1500a`:**
  - `bus-rom:` takes `address: 0x8800`, `pv: 0`.
  - `keys:` gives `NEW0`, PRO mode, a small sample program (GOTO / GOSUB / ON…GOTO / THEN / RESTORE / a quoted `"GOTO 5"`, and a reference that grows from 2 to 3 digits), then back to RUN mode.
  - `debug: command: RENUM 100,,10`.
- **`scaffold.js`:** update the target's label and detail (no longer "entered with CALL"). Keep the id `pc1500-bus-rom` and the file name `rom`.
- **`templates/pc1500-bus-rom/gitignore`:** unchanged.

### Step 4: Docs
- **`docs/Debugger.md` §3:** change the table row and the text ("`CALL &8000` for the minimal template" becomes `RENUM`), and add a short walkthrough of the RENUM project: create it, F5, a breakpoint on `RENFIX`, step, Build & Load, `boot: debug` to stop in `INIT`.
- **New `docs/background/PC1500-Keyword-Modules.md`** (background level, with a row in `docs/background/README.md`): the page layout, the index, the entry format, the code bytes, the entry/exit convention and the boot init call. It is checked against the ROM, cites ROM addresses, and credits Paul Chambre's analysis.
- **The plan itself:** `docs/background/plans/PC1500-RENUM-ROM-Plan.md`, with a row in the plans table.
- **`vscode/calcu1600-debug/README.md`:** update the template description if it mentions CALL.

### Step 5: Regression
In `tools/dap_smoke.py`, add a case next to `bus_rom_run()`:
- scaffold the PC-1500 ROM template into a temp folder, build it with sdas, and start with `command: RENUM 100,,10`;
- check that a function breakpoint on `RENUM` stops in the source;
- continue, then check the renumbered program: the `--dump-basic` equivalent, or read the program memory through DAP.

The existing `busrom.asm` fixture test stays.

## Verification
1. **Build:** `sdaslh5801 -plosgff rom.asm && sdld -nf rom && makebin …` (the extension's own recipe) builds without warnings.
2. **Headless:** `headless/pc1500_cli --preset <scaffolded>/debug.pc1500a`, then type `RENUM 100,,10`, then `--dump-basic`. Check:
   - the line numbers and every reference, including ones that grew a digit;
   - that the quoted `"GOTO 5"` is untouched;
   - `RENUM` with the PC-1600's defaults and with an `old` start;
   - the overflow error, with the program unchanged;
   - `GOTO 100+X`: error, program unchanged;
   - a missing target: reported, the rest renumbered;
   - `RENUM` inside a program: error;
   - `PRINT 1` afterwards still works, with no leftover state.
3. **Compare against the PC-1600:** run the same program and the same `RENUM` arguments in `headless/pc1600_cli`. The resulting line numbers must match.
4. **Coexistence:** a preset with `interface: CE-150` and with `CE-158` still boots, the CE commands work, and so does `RENUM`.
5. **VS Code (user check):** Create Debug Project → PC-1500 ROM extension, F5, stop at a breakpoint in `RENFIX`, step, Build & Load after an edit, and `boot: debug` stops in `INIT`.
6. **Smoke test:** `python3 tools/dap_smoke.py` passes.

## Critical files
- `vscode/calcu1600-debug/templates/pc1500-bus-rom/{NAME.asm,debug.pc1500a}`, `vscode/calcu1600-debug/scaffold.js`
- `tools/dap_smoke.py`
- `docs/Debugger.md`, `docs/background/README.md`, the new `docs/background/PC1500-Keyword-Modules.md`, the new `docs/background/plans/PC1500-RENUM-ROM-Plan.md`
- Reference only: `PC1600-P1-B3B.asm` (RENUM), `PC-1500_ROM-A0x.lh5801.asm`, `CE-150.lib`
