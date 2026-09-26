# Code Cleanup Backlog Plan (low-risk part)

## Context

TODO.md's "Code cleanup backlog" has ten refactor entries. A first plan tried
to fix all ten. On review, six of them would mostly *move* the problem
somewhere else, where the next simplification run would find it again. Those
need analysis or ROM checks first. So this plan does only the four low-risk
items, and rewrites the six risky TODO entries to say what has to be done
first.

Rules for the work:
- The emulator's behaviour and emulated timing must not change.
- Each phase is committed separately on `dev-0.6.0` and removes or rewrites
  its TODO entry in the same commit (TODO.md is committed whole).
- Phases run back to back without check-ins.
- First step: copy this plan to `docs/Code-Cleanup-Plan.md` and commit it.

## Phase 0: rewrite the six risky TODO entries

Rewrite each entry so it leads with "Before fixing:" and the required
analysis, then states the target fix. The cost description stays.

- **TC8576F interrupt plumbing.**
  - Before fixing: check how SC7852 samples INT (`setIntLine` push vs a
    per-instruction read) and what a pull-model level read costs on the hot
    path.
  - Target: nothing pushes updates. Moving the four refresh wrappers into
    `PC1600Memory` would only move them.
- **PC-1600 program pointers.** Moved to its own TODO chapter ("PC-1600
  program loading and pointer bookkeeping"): several load scenarios have to
  be traced on the ROM before deciding.
- **Card/floppy template-vs-instance rules.**
  - Before fixing: analyse `MemoryCardDefinition`. Cutting the text at a key
    depends on key order, which hand-written `.card.yaml` files don't
    guarantee, and `isRom()` may need the regions.
  - Likely target: a parse mode that skips decoding `initial-content` hex.
  - The `ResolvedSource {path, isTemplate, battery}` and the shared catalogue
    helper follow from that.
- **Picker and peripheral-button plumbing.**
  - Design constraint: the "already checked" guards must end up as one guard
    inside the shared picker (call onPick only when the value changes). Don't
    replace them with `toggled(true)` plus `QSignalBlocker`s, which only moves
    them.
  - Enum, warning and status-helper parts as before.
- **Connectors.** Moved to its own TODO chapter ("Expansion connectors: one
  model on both machines"): five connector paths, not two; research the
  PC-1600 60-pin signals first.

## Phase 1: leftovers (TODO: "Leftovers, one quick pass")
- Delete `PC1600LhsWindow`, `pc1600LhsWindow()` and `PC1600Bank::lhsRemapRow()`
  (`Core/PC1600/PC1600Bank.hpp`) and their test in
  `Core/tests/pc1600_bank_tests.cpp`. Keep the remap table as a comment on
  Port 31H b6.
- `Core/Connector/Ce158Card.hpp`: `kCpuHz = kPC1500CpuHz` from
  `Core/PC1500/PC1500Clocks.hpp`. Connector already includes PC1500 headers.

## Phase 2: banked/unbanked normalised at parse time
- `Core/Connector/MemoryCardDefinition.hpp`: after a region is parsed, always
  fill `banking.bankCount` / `banking.bankSize` (unbanked = 1 × `capacity`)
  and `capacity` (banked = count × size). The `banked` flag stays, meaning
  "has a bank latch".
- First, check every reader of `banked` / `banking.*` to confirm none uses
  `bankCount == 0` or `bankSize == 0` as a stand-in for "unbanked".
- Collapse the `r.banked ? … : …` ternaries in `SoftwareDefinedCard.hpp`
  (~91, 102, 203, 340-341) and `MemoryCardDefinition.hpp` (166, 1087-1088,
  1289, 1318-1319), plus the test helper in
  `battery_card_instance_tests.cpp:385`.
- The `.card.yaml` text format (`BatteryCardInstance.hpp`'s negative bank
  count for unbanked) is unchanged.

## Phase 3: lazy LCD edges (timing-neutral part of the LCD/sub-CPU item)
- `PC1600Display::tick()` only adds to a `uint64_t m_tstates`, with no loop.
- `m_lcdEdges` is replaced by `lcdEdges()`, which computes
  `m_tstates * kLcdClockHzTimes6 / kTStateHzTimes6` with an `unsigned
  __int128` product (a 64-bit product overflows after about 7 days of emulated
  time). It is used only in `readIO` / `writeIO` (`PC1600Display.cpp` 51/94).
- Reset clears `m_tstates` exactly where the accumulator and edges are reset
  today.
- The TODO entry keeps the re-fit and per-command response-time part, pointing
  to the speed-gap item.

## Phase 4: CLI and tool-script helpers
- New `tools/CliCommon.hpp` with:
  - `readFile` (whole file, using `BundledRoms::detail::readWholeFile`);
  - `readFileExact(path, size)` for pc1600_cli's 16K ROM reads, which need an
    exact size;
  - `writeFile(path, text)`;
  - `printCe150Report(...)`.
- Use it in `tools/pc1500_cli.cpp` (~303), `tools/pc1600_cli.cpp` (57, 117,
  159) and `tools/Ce158CliPeer.hpp:67`.
- New `tools/build_app.sh` (configure if needed + build). `build_and_run.sh`
  and `tools/make_screenshots.sh` call it.
- Update the `tools/build_*` scripts if they need to know about the new
  header.

## Verification (every phase)
- `sh tools/run_tests.sh`: the whole Core suite passes.
- Qt build: `cmake --build Qt6/build` (after phase 4, via
  `tools/build_app.sh`).
- CE-150 regression: `tools/pc1500_cli --preset examples/ce150_demo.pc1500a
  --ce150-rom roms/CE-150.ROM 400000000` still gives **1254** plot points.
- Phase 2: memory-card, battery-card-instance and superRAM card tests; a
  CE-1601M preset (`examples/maxed-out-mem.pc1600`) still runs.
- Phase 3: `pc1600_cli --preset` of a scrolling-PRINT program and
  `examples/lissajou.pc1600` give identical output and cycle counts before and
  after (capture before the change).
- Phase 4: both CLIs build via their scripts and give identical `--preset`
  output. `tools/make_screenshots.sh` still builds and runs.
- At the end, TODO.md's cleanup section holds:
  - the six rewritten "Before fixing:" entries;
  - the LCD/sub-CPU re-fit remainder.

## Outcome (2026-09-26)

All four low-risk phases landed as planned, with two small deviations:
- Phase 3 computes `lcdEdges()` as quotient plus remainder instead of an
  `unsigned __int128` product. It's exact, can't overflow, and builds with
  MSVC too. The scrolling-PRINT benchmark's BEEP audio stayed bit-identical.
- Phase 4: `pc1600_cli` now prints the CE-150 event list like `pc1500_cli`
  (one shared `cli::printCe150Report`). All other CLI output is unchanged.

The six "Before fixing" items remain in TODO.md.
