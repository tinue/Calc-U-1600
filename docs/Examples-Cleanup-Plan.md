# Clean up `examples/` — user-facing only

## Context
`examples/` is zipped as-is into the release (`bin/release:585` → `Calc-U-1600-examples.zip`) and the README/User Guide point users at it. Today it mixes real learning material (Lissajous, biorhythm, memtest, Calculat…) with hardware-verification probes, loader-debug presets and VS Code debugger plumbing, all in a flat directory. Goal: `examples/` holds only material useful for learning or using the emulator, grouped by topic with an index; everything else moves to where it belongs. All moves via `git mv` (history kept); no signal/core behaviour changes.

## Target layout

### `examples/` (shipped)
| Dir | Contents (with their `.bas`/`.bin` companions) |
|---|---|
| `plotter/` | `lissajou-1500`, `lissajou-1600`, `lissajou-ce150` (+`lissajou-ce150-1600.bas`), `biorhythmus_1600`, `globus`, `ascii`, `ce150_demo.pc1500a`, `plotter_150_test` / `plotter_1600_test` → rename `ce150-text-and-frame.pc1600` / `ce1600p-text-and-frame.pc1600` (they show CSIZE sizes + max plot area — useful, so kept) |
| `basic/` | `hanoi`, `dampflok.bas` |
| `interfaces/` | `ce158_demo.pc1500a`, `ce158_demo.pc1600`, `setcom1600` |
| `machine-code/` | `memtest.asm`, `memtest_bank.asm`, all `memtest_*.bin/.pc15xx`, `Calculat.pc1600` + `CALCULAT.BIN` (from `assembly/`), `DiskWorks.pc1600` + `dwx.bin` |
| `memory/` | `flashtest_ce163f.pc1500a`, `maxed-out-mem.pc1600`, `memory-cards/` (moved as subdir) |
| `startup/`, `setup/` | unchanged |
| `README.md` (new) | one-line-per-file index by topic, how to open a preset / load a `.bas` |

### `vscode/presets/` (new)
`debug-pc1500a.pc1500a`, `debug-pc1600.pc1600` — the "Debug on …" clean-start presets.

### `dev/` (new top-level, tracked, not shipped) + `dev/README.md`
- `dev/hardware-checks/` — programs run on real hardware to settle emulator questions: `adrtest.*`, `adrtest_1500a.*`, `instrquirks*`, `lcdallon*` (asm + bin + preset).
- `dev/presets/` — debug/regression presets: `iterator.pc1500`, `loader_test_1600_memory_cards.pc1600`, `supercard_debug.pc1600`, `trace_demo.pc1500a`.

### Tests stop depending on `examples/`
- Copy `memtest_stock.bin` into `Core/tests/fixtures/listings/sdas-lh5801/` (its `memtest.asm`/`.rst` already live there; asm is identical) and point `Core/tests/listing_tests.cpp`, `run_control_tests.cpp`, `tools/dap_smoke.py` at it.
- `Core/tests/basic_binary_image_tests.cpp:162` references non-existent `examples/lissajou-*_tokenized.bas` → repoint to `Core/tests/fixtures/…` (still skip-if-absent).

## Reference updates (after the moves)
- Intra-preset `path:`: `setup/make_diskworks_{card,floppy}.pc1600` (`../globus.bas` → `../plotter/globus.bas` etc., `setcom1600.bas` → `../interfaces/`); `dev/presets/loader_test…` → `../../examples/plotter/globus.bas`; fix case `CE1638_BANKSWRM_FIXED.bin` in `setup/ce1638_bankswrm.pc1500a`.
- Header comments with CLI lines (`ce158_demo.pc1500a`, `trace_demo`, `memtest.asm`, `instrquirks_1500a.asm`) and their fixture copy comment.
- Docs: `docs/User-Guide.md` (440, 540–541, 553, 569, 664, 671), `docs/Debugger.md:80-81`, `docs/PC1600-Core-Limitations.md:256`, `docs/Memory-Card-Definition-{Spec,Format}.md`, `THIRD-PARTY-NOTICES.md:42-47`, `README.md:10-16`, `headless/README.md:12` (mention `dev/`), `docs/Up-Down-Key-Investigation.md` if it names adrtest paths. Historical plan docs (`Code-Cleanup-Plan`, `Debugger-Restructuring-Plan`, `DAP-Debugger-Plan`) left as-is — they record past state.
- Screenshots: `docs/screenshots/presets/pc1600-{lissajou,hanoi-list,hanoi-run}.pc1600`, `docs/screenshots/guide/06-machine-code.shots.yaml`, `08-presets.shots.yaml`.
- VS Code: `vscode/workspace/launch.json:16,33`, `vscode/calcu1600-debug/README.md:79` → `vscode/presets/…`.
- `.run/pc1600_cli.run.xml` → `examples/plotter/lissajou-1600.pc1600`; `.run/pc1600_plotter_probe.run.xml` points at non-existent `plotter_test.pc1600` → `examples/plotter/ce1600p-text-and-frame.pc1600`; same stale name in `tools/build_pc1600_plotter_probe.sh:10`.
- Stale comments: `Core/Preset/PresetFile.hpp:84`, `Core/CPU/LH5801/LH5801.cpp:752`, `Core/tests/lh5801_tests.cpp:172`, `Core/tests/basictyper_tests.cpp:90`.
- `.gitignore:70-77`: `/examples/memtest.bin` → `/examples/machine-code/memtest.bin`; add same build-artifact globs for `/dev/**`.
- `docs/Decisions.md`: add "examples/ is user-facing only; probes → dev/, debugger presets → vscode/presets/, tests use Core/tests/fixtures".

## Commits (on dev-0.6.0)
1. Move dev material → `dev/`, debugger presets → `vscode/presets/`, fix their refs.
2. Tests/tools onto fixtures.
3. Regroup `examples/` by topic + README + all ref updates.
4. Decisions.md entry.

## Verification
- `grep -rIn "examples/" --exclude-dir={build,cmake-build-debug,source-material,.git}` — every hit resolves to an existing file (script check).
- `tools/run_tests.sh` green (listing/run-control tests read the fixture, not examples).
- Headless: `pc1500_cli --preset examples/plotter/lissajou-1500.pc1500`, `pc1600_cli --preset examples/plotter/lissajou-1600.pc1600`, `examples/setup/make_diskworks_card.pc1600`, `examples/memory/memory-cards/pc1500-maxed-out.pc1500`, `examples/machine-code/memtest_stock.pc1500` all load without path errors (outputs into `headless/`).
- `tools/make_screenshots.sh` dry run of chapters 06/08 + the hanoi/lissajou presets resolve their files.
- `git ls-files examples` contains nothing from the dev list.
