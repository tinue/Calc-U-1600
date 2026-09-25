# Debugger — handoff (2026-09-25)

Where the DAP debugger stands at the end of the first working session, what was verified, what the user reported from VS Code, and what to pick up next. Read this together with:
- `docs/Debugger.md`: user documentation, including how it works;
- `docs/DAP-Debugger-Plan.md`: the plan, with a status note on how the implementation differs.

## State

All work is committed on `dev-0.6.0`:

| Commit | What |
|---|---|
| a7b062b | Phase 1: always-on history rings, LH5801/Z80 disassemblers |
| 7d26a2a | Phase 2: CPU/machine hooks (SC7852 breakpoints, skip-once, per-CPU memory watches, ME1 peeks), `DebugTarget`, expressions |
| c12904d | Phase 3: listing parsers (sdas 16/32-bit layouts, zasm), `SourceMap` |
| 3a784e4 | Phase 4a: `BreakpointTable`, `RunControl` (Core) |
| 1cb5afc | Phase 4b: `DapServer`/`DapSession`/`DebugController`, Settings ▸ Debugger, `--dap` |
| c43036a | Phase 5: Build & Load, reset/restart, attach `preset` |
| 56f4489 | Clean start + inbound typing (`MachineController::typeCommand`) |
| d340fc4 | Phase 6: VS Code extension, docs, smoke test |
| b01b4c6 | macOS: window restoration off (startup deadlock) |
| 92e927a | Extension installed as a `.vsix` (`tools/install_vscode_extension.sh`) |
| a930b14 | Settings ▸ Debugger ▸ Disconnect; clearer "already attached" message |
| ddef5ea | Numeric memory references (disassembly view), Banks under Registers |

**Verified:**
- **CoreTests:** fully green, including the new suites:
  - `disasm_tests`: opcode sweeps against both CPU cores and the sdaslh5801 table;
  - `debug_target_tests`;
  - `listing_tests`: real assembler fixtures in `Core/tests/fixtures/listings/`;
  - `run_control_tests`: memtest with its listing.
- **End to end:** `uv run tools/dap_smoke.py --app build/Qt6/Calc-U-1600.app/Contents/MacOS/Calc-U-1600` passes all four scenarios over real DAP:
  - a plain session;
  - Build & Load on a PC-1500 (memtest);
  - Build & Load on a PC-1600 (the zasm ROM dumper);
  - ROM reset-and-step.

  It launches with `-ApplePersistenceIgnoreState YES` and quits via `calcu1600/quit`.
- **Regressions:**
  - Plotter output is identical before and after (PC-1500 CE-150 demo, lissajou, PC-1600 lissajou).
  - The always-on history costs about 10% in raw headless speed (3.9 s → 4.27 s for 3 billion PC-1600 cycles). That's negligible at 1× real time.

## Reported by the user in VS Code, not yet confirmed as fixed

The fixes are in ddef5ea, and the smoke test passes. The user still needs to rebuild their app (their build lives in `Qt6/build/`) and try again in VS Code:
1. **The Disassembly view was empty.** Cause: memory references like `1:0000`, which VS Code can't parse as a number. They are now numeric. Check that the view shows code and marks the current instruction, and that instruction breakpoints set in the view work.
2. **The Banks scope wasn't visible.** It is now an expandable *Banks* entry under *Registers* (live frame only). Check that it shows.
3. **"Paused on pause" instead of "entry"** for the ROM configuration. A scripted replay of the same requests gives `entry`. Probably the Pause button, or the older app build. Check again with the rebuilt app.

Other user-facing notes from the session:
- **CMake Tools kit prompt:** opening the repo root in VS Code activates the CMake Tools extension, and its status-bar Build/Debug buttons ask for a kit. That isn't the debugger. Suggested: pick `[Unspecified]` once, or keep assembly projects in their own folder. The user was offered a `.vscode/settings.json` that turns the prompt off; they haven't answered.
- **Installing the extension:** current VS Code ignores extensions symlinked into `~/.vscode/extensions`. Install with `tools/install_vscode_extension.sh`, which packages the `.vsix` into `headless/`, then reload the window.
- **"LH5801/PC-1500"** in VS Code's debugger list comes from the separate `pchambre.lh5801-asm` extension, not from ours.

## Where the code is

- **`Core/CPU/`:**
  - `HistoryRing.hpp`: the per-CPU history, recorded in `step()`;
  - `WatchSet.hpp`: memory watches;
  - breakpoint / skip-once / watch hooks in `LH5801` and `SC7852`.
- **`Core/Debug/`:**
  - `Disasm/`;
  - `DebugTarget` + `MachineDebugTargets`: the PC-1500 / PC-1600 adapters;
  - `CpuRegisters`;
  - `DebugExpression`;
  - `Listing/`: the `listingFormats()` table is the extension point for TASM or library listings later;
  - `SourceMap`, `BreakpointTable`, `RunControl`;
  - `ProgramLoader`: Build & Load.
- **`Qt6/app/debug/`:**
  - `DapServer`: transport;
  - `DapSession`: the requests;
  - `DebugController`: owned by `MachineController`.
- **Hooks elsewhere:**
  - `MachineController::runActive()`: routes each frame through `runSlice()` while attached;
  - `discardMachine()`;
  - `typeCommand()`;
  - `MainWindow::runSynchronousLoad()`: `setAppBusy`;
  - `main.cpp`: `--dap`, `macDisableWindowRestoration()`.
- **`vscode/calcu1600-debug/`:** the extension. Install with `tools/install_vscode_extension.sh`.

## Design rules learned the hard way

- **Arming:** breakpoints and watches are armed only inside `DebugController::runSlice()`. A boot (`runBootToPrompt`), a preset or a load drives the machine directly and must never park on a breakpoint; those loops would hang.
- **Queueing:** DAP messages wait while the app runs a synchronous load (`setAppBusy`). An attach that arrived during the startup preset once rebuilt the machine under the running loader and aborted the app.
- **Typing:** the GUI paste never presses ENTER; that's deliberate. Tools use `typeCommand()`, which does. `enqueueKey()` is PC-1500 only.
- **Build & Load order:** clean start (the configuration's `preset`, else the model's default preset, else All Reset), then a direct load (no dialog or popup), then auto-start. `cleanStart: false` skips the clean start.
- **Memory references:** must stay numeric; see `DapSession::memoryReference()`.
- **Scripted launches:** never SIGTERM the app. macOS then shows a restore prompt, which used to deadlock the startup preset. The app now opts out of window restoration, but the smoke test still quits cleanly.

## Next steps

1. The user checks items 1–3 above in VS Code with a rebuilt app, and does the manual checklist in `DAP-Debugger-Plan.md` ▸ Verification:
   - conditional breakpoint;
   - step over a `SJP`;
   - edit a register;
   - data breakpoint;
   - memory view;
   - PC-1600 LH5803 handoff.
2. Decide on the CMake Tools `.vscode/settings.json` question.
3. **Known limitations** (listed in `docs/Debugger.md`):
   - no PC-1600 vertical-bank qualifier;
   - no ME1 writes;
   - no BASIC start for LH5803 code;
   - the zasm problem matcher only takes the first error per file section;
   - CLion is not set up yet.
4. **Later:** TASM ROM-disassembly listings (e.g. Jeff Birt's) as another `ListingSource` parser.
