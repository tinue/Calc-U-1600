# Debug panel → INSPECTOR: pointers, memory views, dumps

> **Status (2026-10-10): implemented.** Differences from the plan below:
> - The Core module is `Core/Debug/Inspect/` (`Inspector`, `TextTable`,
>   `HexDump`, `PC1500Inspector`, `PC1600Inspector`) instead of
>   `MemoryViews`. The pointer tables moved there; `BasicPointerTable` keeps
>   only `readPC1600ProgramAreas()`.
> - No list of ROM images: the inventory names the built-in ROMs itself and
>   asks the cards (`CardBase::debugMemories()`, `moduleName()`) and the bus
>   (by offering cycles) for the rest. Bus ROMs are named after their file.
> - F055H/F05BH turned out to be the module's non-disk RAM, not the disk
>   size (ROM P1-B3 4B9A–4BBC); a RAM disk is the rest of the module and
>   counts only with its boot sector (docs/background/Decisions.md, GUI).
> - The CLIs got `--inspect <view>`, which the plan only had as a
>   possibility. The screenshot scenarios can open the menus (`open:`), not
>   pick an entry.
> - Not done: RDSKS1/RDSKS2 fields (the layout isn't settled), the
>   RAM-disk directory listing (TODO.md).

## Context
The "DEBUG LOG" panel (`Qt6/app/DebugPanel.*`) was meant to grow into a debugger. That
job now belongs to the DAP debug interface, so the panel becomes a read-only
**machine inspector**. Three things change:
- **Pointers**: the current lists are short, and the PC-1500 list has errors. The research corpus has many more.
- **Dump Mem**: today one long PC-1600 printout mixes the bank map, a resource grid and an internal-RAM hex dump. It is replaced by separate table views and dumps that each answer one question.
- **Naming**: the header and the buttons get new names (decided: INSPECTOR).

Decisions made with the user:
- Header "INSPECTOR".
- Button bar: two drop-down buttons, `Memory ▾` and `Dump ▾`.
- The PC-1500/1500A gets a matching subset of the views.
- Disk areas: geometry, boot-sector header and raw dump, but no file listing.

## Architecture
The view logic moves into Core so it is unit-testable (there are no Qt tests) and can
later be reused by the CLI and DAP.
- **New `Core/Debug/MemoryViews.{hpp,cpp}`** builds structured models: `InventoryRow`, `CpuViewRow`, `AreaSegment`, …
- **New `Core/Debug/TextTable.{hpp,cpp}`** is a small box-drawing table renderer (`┌─┬─┐ │ ├─┼─┤ └─┴─┘`, column widths from the content). The views render through it.
- **`DebugPanel`** keeps only the UI: menus, the ring buffer, and `ringWriteAll(render(...))`. The old `resourceCell`, `targetName`, `debugDumpMemRegion` and `hexRowsFrom` code moves into Core or is removed. One shared hex dumper in Core keeps today's skip-00/FF-run behaviour and takes a base address and a label.
- **Data access** goes through new locked `PC1600Machine` / `PC1500Machine` debug accessors, passed through `MachineController`, following the existing `debugBankState` / `debugSlotImage` pattern. Every read is side-effect free.

## Phase 1 — Rename + button bar
- Header `DEBUG LOG` → `INSPECTOR`. Update the class comment and `docs/User-Guide.md` §9.2.
- Row 1: `Pointers`, `Memory ▾` (QToolButton + QMenu, InstantPopup), `Dump ▾`, stretch, `Card YAML` (renamed from "Dump Card YAML"), Clear.
- Menu entries are rebuilt on `aboutToShow`, so they depend on the model and on what is attached (PC-1500 entries are hidden; S1/S2 entries are greyed out when the slot is empty).
- Object names `debugpanel.memory` and `debugpanel.dump`. Update `docs/developer/screenshots/README.md` and the scenarios that click `debugpanel.dumpmem`.
- LOG pill: leave it as is (out of scope).

## Phase 2 — Pointers (`Core/Debug/BasicPointerTable.cpp`)
Output is grouped, one heading per group, with decoded flags where it helps.

**PC-1600**, adding to the existing entries (addresses from the research corpus; check
each one in `pc1600/notes/*-Symbols.md` while implementing):
- **Program / execution:**
  - PROCPTR FE00 (LE)
  - PRGSTA FE3C–FE3E and PRGENDA FE3F–FE41 (3 bytes each, current TITLE area; check the byte order in `PRGADR` 02F4H)
  - LBCUR F1C1
  - TRON F88D and TRONMODE F88E
  - BREAK/ERROR/ON ERROR line+top F8AE/F8B0/F8B4/F8B6/F8BA/F8BC
  - CONTADR F0B9 (LE)
- **Stacks / evaluator:**
  - FORPTR F890, GOSUBPTR F891 (low bytes)
  - STRBUFP F894
  - live Z-80 SP against F500–F5FF
- **Program areas / ML reserve:**
  - S0DESC F029 and S0MTb F02A
  - ADTBL F1D6–F1DA, decoded (leading-bank flag / bank / slot)
  - ML reserve = BASPRG_ST − (F029&7F):C5 (computed)
- **Files / RAM disk:**
  - FBNO F02D
  - PTRTAB F02E–F04F slices, shown only when PTRn ≠ PTRn+1
  - F055/F05B RAM-disk size (×2 KB)
  - RDSKS1/2 FC00/FC08 (media ID, open files)
  - DEVNAME FC16
- **Mode / system:**
  - BMODE F1BC (MODE 0/1, BREAK OFF)
  - SYMB1 F64F (RUN/PRO/RESERVE)
  - F07D b2 (BASIC ROM 3 / 3b)
  - SLOTMAPM F08D
  - RSTCAUSE FA1B
  - BINTREQ F127 and BINTEN F12A
  - OPNDV F9D1
- **Derived values:**
  - S0 program size
  - variable area used = RAM_END:00 − VARPTR
  - free gap
  - MEM / STATUS 259/260, as today
- **Fix:** the description of ON_ERR_ADDR ("bit 15 set: off") has no source. Check it in the ROM; if it can't be confirmed, drop the note.
- **Keep:** CRSRX/CRSRY stay as in the EQU labels; the TRM swaps them, so note the disagreement in a comment.

**PC-1500**:
- **Fixes:**
  - BREAK_STAT 7881 is wrong (it is the evaluator element type). Replace it with BREAKPARAM 788A.
  - WARM_START 7A20 and STK_SAVE 7A21 lie inside FP register ARV, so drop them.
  - DISP_CTRL 7880 becomes "DISPARAM: display at READY".
- **Add:**
  - CURR_TOP 789E
  - ERL 789B
  - BRK_*, ERR_*, ON_ERR_* line/top (78AC–78BC)
  - FOR/GOSUB stack pointers 7890/7891
  - STR_BUF_PTR 7894
  - mode annunciator 764F (RUN/PRO/RESERVE)
  - PU_PV 79D0
  - OPN 79D1
  - ML reserve and variable-area derived values
- **When attached:**
  - CE-150 group: 79E0–79F4
  - CE-158 group: 7850–7858

Tests: extend `test_debug_program_areas_match_rom` (`Core/tests/pc1600_program_placement_tests.cpp`) for the new derived values.

## Phase 3 — Core accessors (prerequisites)
In `PC1600Machine` (locked) → `MachineController`:
1. `debugRomImages()`: system ROM lo/hi, bank 3, 3b, 6, LH5803 C000–FFFF, plus CE-1600P (`CE1600PCard::debugRomImage`), host drive bank 7, preset bus ROMs. Name and size for the inventory; the bytes are only needed when a dump asks for them.
2. `debugBusCards()`: walks `SystemBus::chain()` and returns, per card, the name, kind, window/bank, PV/PU, and ROM/RAM size. Covers CE-150, CE-158, CE-1600P, host drive and bus ROMs, on both models.
3. `debugSlotRegions(slot)`: from `SoftwareDefinedCard::definition().regions[]`, gives kind (RAM/ROM/flash), size, bank count/size and the trigger (Pin vs port 28H = vertical).
4. `debugReadSlotBus(slot, bank, addr)`: built on `MemorySlotConnector::readInBank`. A pure read; `slotBusWritable` does a probe write, so it must not be used here.
5. Fix `debugBankState()`: page B banks 6/7 are reported as `OpenBus` although `PC1600Memory::read` sends them to the system bus. Add `DebugPageTarget::BusCard`, which reports the host drive or bus ROM. Report empty slots as `OpenBus`. Check CE-1600P presence in Core instead of in the panel.
6. LH5803: add a locked `debugLh5803Map()` with PU, PV, and per 4K/8K window which card answers in ME0/ME1. Also a locked `debugLh5803Peek`. Fix `LH5803SharedMemory::debugPeek` so ME0 applies `lha90()` the way `readME0` does. Read `docs/background/Decisions.md` first, including the F000 latch entry at :37, whose "plain latch" text is also out of date compared with the code.
7. `debugS0Segments()`: `planS0Placement(in, {})` returns the segment list (kind, slot, ADTBL index/bank, window base, top). Reuse `pc1600SlotGeometry` from `PC1600MachineCodeLoader.hpp`.

PC-1500 (`PC1500Machine`): existing `debugSlotCard*` and `debugSlotResponds`, plus `debugBusCards()` and a PU/PV snapshot.

Tests go in `Core/tests/pc1600_machine_tests.cpp` next to the existing "GUI debug panel" tests:
- bus card in page B bank 7 (host drive)
- pure slot read doesn't bump `contentRevision`
- `lha90` peek matches the CPU read

## Phase 4 — Memory ▾ views (`MemoryViews`)
1. **Inventory**: every physical memory present, one table:
   ```
   ┌──────────────┬──────────┬───────┬──────┬─────────────────────────────┐
   │ Resource     │ Kind     │ Size  │ Banks│ Seen at                     │
   ├──────────────┼──────────┼───────┼──────┼─────────────────────────────┤
   │ System ROM   │ ROM      │ 32K   │ 1    │ Z80 A b0/1, B b0            │
   │ BASIC ROM 3  │ ROM      │ 16K   │ 1    │ Z80 B b3 (3DH b2=1)         │
   │ Internal RAM │ RAM      │ 16K   │ 1    │ Z80 D · LH5803 ME0 4000-7FFF│
   │ Slot 2 CE-1601M│ RAM    │ 64K   │ 4 vert│ Z80 C b2/3 (28H)           │
   │ CE-1600P     │ ROM      │ 32K   │ 2    │ Z80 B b4/b5                 │
   └──────────────┴──────────┴───────┴──────┴─────────────────────────────┘
   ```
   Sections: built-in / memory slots / 60-pin bus.
2. **Z80 View**: replaces "address map (live)". Rows for pages A–D: range, bank, source, offset into the source, RAM/ROM. A footer shows ports 31H/28H/3CH/3DH plus SLOT1MAP/SLOT2MAP redirects; this replaces the old text and the "armed" grid.
3. **LH5803 View**: ME0 and ME1 windows (0000–7FFF → Z80 +8000, i.e. which physical RAM; 7400/7500 LHA90 alias; 7600–764F LCD mirror; 8000–BFFF bus card per current PU/PV; C000–FFFF private ROM; ME1 I/O holes). PC-1500: **LH5801 View**, same layout.
4. **BASIC Area**: the S0 segments from bottom to top, as in the ROM's ADTBL order. Columns: segment, physical source (slot/bank/offset or internal RAM), Z80 and LH5803 address, size. Markers for ML reserve, BASPRG_ST, BASPRG_END, the free gap, VARPTR and RAM_END. Total ≈ MEM. PC-1500: RAM_ST…RAM_END with the same markers.
5. **Program Areas**: S0/S1/S2 (only those that exist; SxMTb FE = folded into S0). Columns: source, start/end/limit, size, free, and which one TITLE has selected.
6. **Disk Areas**: S1:/S2:. Size from F055/F05B; the slot/card regions it occupies; for S2: the vertical banks (port 28H) and which bank is latched now; the boot-sector header (media ID, MAXCLS, MAXDIR, FAT location). The offsets follow `superram.card.yaml:95–125` and `PC-1600-Filesystem.md`; check them against the romIII-3 geometry table. Read through `debugReadSlotBus`.
7. **Physical RAM** (the extra view): reverse map per physical RAM (internal 16K, each slot bank and vertical bank), listing which role holds which byte range: work area F000–FFFF, S0 BASIC, ML reserve, variables, S1 program area, RAM disk, unused. This answers "who owns this chip?", which none of the other views do.

## Phase 5 — Dump ▾
Hex dumps through the shared Core dumper. Addresses are shown in the CPU view, with the physical source in each section header.
- BASIC Area (S0, segment by segment)
- Program Area S0/S1/S2: only the existing ones, BASPRG_ST..END plus the FF mark
- Disk S1: / Disk S2: (all vertical banks of S2:, with a bank heading for each)
- PC-1500: BASIC area plus the ML area 7C00–7FFF

## Phase 6 — Docs + commit
- `docs/User-Guide.md` §9.2 rewritten (INSPECTOR, menus, one sentence per view); CHANGELOG.
- `TODO.md:279–285` (the pointer review) marked done. `TODO.md:286` (RAM-disk contents) stays open for the file listing.
- Save this plan as `docs/background/plans/Inspector-Panel-Plan.md` and add a row to `docs/background/README.md`.
- Commit each phase to `dev-0.8.0`.

## Verification
- `tools/run_tests.sh`: new Core tests for the accessors, the views (rendered text against fixed machine states: default PC-1600, CE-1601M in S2 plus `INIT"S2:"`, a program module in S1, a TITLE switch) and the table renderer.
- Headless: a small probe in `headless/` that prints each view for a few `.pc1600` presets (program-area-tests from the research corpus). Check the numbers against `MEM` / `STATUS 259/260` read from the LCD (`--lcd-text -`).
- GUI: build the app (`tools/build_app.sh`), run each menu entry on PC-1600 and PC-1500 (with CE-150, CE-158, CE-1600P, mounted directory), and check light/dark themes and the narrow-bar layout. Launch with `-ApplePersistenceIgnoreState YES` and quit via `calcu1600/quit`.
