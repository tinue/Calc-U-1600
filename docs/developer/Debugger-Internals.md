# Debugger — internals

How the DAP debugger is built, and the rules its code relies on. The user
manual is [../Debugger.md](../Debugger.md); the design history is in
[../background/plans/DAP-Debugger-Plan.md](../background/plans/DAP-Debugger-Plan.md)
and [../background/plans/Debugger-Use-Cases-Plan.md](../background/plans/Debugger-Use-Cases-Plan.md).

## Behaviour not in the manual

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
