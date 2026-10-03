# Debugger — handoff

What is still open on the DAP debugger, and the internals that aren't in
the user manual. Read it together with:
- `docs/Debugger.md`: the user manual (installation, setup, the four use cases, reference);
- `docs/background/plans/DAP-Debugger-Plan.md`: the plan, with a status note on how the implementation differs;
- `docs/background/plans/Debugger-Use-Cases-Plan.md`: the IDE integration by use case.

## Open: checks in VS Code

The automated side passes: CoreTests, `tools/run_tests.sh` and
`tools/dap_smoke.py` (plain session, Build & Load on both machines, ROM
reset-and-step, restart, All Reset & Stop, project preset, PC-1500 bus ROM
by `command`, host-drive ROM as a bank-7 bus ROM, `boot: debug`). These
need a person in VS Code with a current app build:

1. **Disassembly view:** shows code, marks the current instruction, and
   instruction breakpoints set in it work.
2. **Banks scope:** an expandable *Banks* entry under *Registers* (live
   frame only).
3. **ROM configuration stops with reason `entry`**, not `pause`. A scripted
   replay of the same requests gives `entry`.
4. **Dynamic configurations:** they appear in the Run and Debug list, and
   F5 works without a `launch.json`.
5. **Build tasks:** the problem matchers resolve absolute paths.
6. ***Create Debug Project…*** in a real folder, including the
   `launch.json` merge.
7. **Build & Load after an edit.**
8. **The manual checklist** in `DAP-Debugger-Plan.md` ▸ Verification:
   conditional breakpoint, step over a `SJP`, edit a register, data
   breakpoint, memory view, PC-1600 LH5803 handoff.

Local leftovers to delete: the repository's own `.vscode/launch.json` /
`tasks.json` (git-ignored, port 4711). The dynamic configurations replace
them.

**Open question:** opening the repository root in VS Code activates the
CMake Tools extension, whose status-bar Build/Debug buttons ask for a kit.
That isn't the debugger. Either pick `[Unspecified]` once, or commit a
`.vscode/settings.json` that turns the prompt off.

**CLion:** documented in the manual (▸ CLion) but not tried. The spike
checklist is there.

## Internals not in the manual

- **Transport:** the server listens on 127.0.0.1 only and takes one client
  at a time; a second one is told the debugger is busy. `--dap <port>`
  overrides Settings for one run.
- **Run control:** while attached, every emulation frame goes through
  `DebugController::runSlice()`: paused, running with breakpoints and
  watches armed, or stepping in frame-sized pieces.
- **`launch` is `attach`:** `DapSession` treats a `launch` request as
  `attach`, which the CLion recipe relies on.
- **Installing the extension:** VS Code ignores extensions symlinked into
  `~/.vscode/extensions`. `tools/install_vscode_extension.sh` packages the
  `.vsix` into `headless/` and installs it.
- **"LH5801/PC-1500"** in VS Code's debugger list comes from the separate
  `pchambre.lh5801-asm` extension, not from ours.

## Where the code is

- **`Core/CPU/`:**
  - `HistoryRing.hpp`: the per-CPU history, recorded in `step()`;
  - `WatchSet.hpp`: memory watches; `BreakpointSet.hpp`; `DebugStop.hpp`: the machines' stop latch;
  - breakpoint / skip-once / watch hooks in `LH5801` and `SC7852`.
- **`Core/Debug/`:**
  - `Disasm/`;
  - `DebugTarget` + `CpuViews` + `MachineDebugTargets`: the machine-wide part, one view per CPU, the PC-1500 / PC-1600 adapters;
  - `CpuRegisters`;
  - `DebugExpression`;
  - `Listing/`: the `listingFormats()` table is the extension point for TASM or library listings later;
  - `SourceMap`, `BreakpointTable`, `RunControl`;
  - `ProgramLoader`: Build & Load (planning in `Core/MachineCodeFile`'s `planLoad()`).
- **`Qt6/app/debug/`:**
  - `DapServer`: transport;
  - `DapSession`: the requests;
  - `DebugController`: owned by `MachineController`.
- **Hooks elsewhere:**
  - `MachineController::runActive()`: routes each frame through `runSlice()` while attached;
  - `discardMachine()` / `wireNewMachine()`: `machineAboutToChange()` / `machineReplaced()`;
  - `typeCommand()`;
  - `Qt6/app/SyncOperations`: every synchronous load/reset; `busyChanged` drives the DAP queue;
  - `main.cpp`: `--dap`, `macDisableWindowRestoration()`.
- **`vscode/calcu1600-debug/`:** the extension. Install with `tools/install_vscode_extension.sh`.

## Design rules

- **Arming:** breakpoints and watches are armed only inside
  `DebugController::runSlice()`. A boot (`runBootToPrompt`), a preset or a
  load drives the machine directly and must never park on a breakpoint;
  those loops would hang.
- **Queueing:** DAP messages wait while the app runs a synchronous
  operation (`SyncOperations::busyChanged`). An attach handled during the
  startup preset would rebuild the machine under the running loader.
- **Typing:** the GUI paste never presses ENTER; that's deliberate. Tools
  use `typeCommand()`, which does. `enqueueKey()` is PC-1500 only.
- **Build & Load order:** clean start (the configuration's `preset`, else
  the model's default preset, else All Reset), then a direct load (no
  dialog or popup), then auto-start. `cleanStart: false` skips the clean
  start.
- **Memory references** must stay numeric (VS Code can't parse `1:0000`);
  see `DapSession::memoryReference()`.
- **Scripted launches:** never SIGTERM the app; quit via `calcu1600/quit`
  and launch with `-ApplePersistenceIgnoreState YES`.

## Later

- The known limitations listed in `docs/Debugger.md`: no PC-1600
  vertical-bank qualifier, no ME1 writes, no BASIC start for LH5803 code,
  the zasm problem matcher takes only the first error per file section.
- TASM ROM-disassembly listings as another `ListingSource` parser.
