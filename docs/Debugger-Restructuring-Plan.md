# Debugger restructuring plan

## Context

The /simplify pass over `dev-0.6.0` (commit d240b90) left a set of findings that were too big for a cleanup pass. This plan fixes them:
- **Load paths:** four machine-code load paths with diverging rules.
- **Debugger bypasses app setup:** its clean start, reset and program load skip the app's synchronous-load setup (clock seed, persist flush, audio discard).
- **Breakpoints tied to TRACE flags:** the breakpoint enable rides on the TRACE flag bits, so every `setTraceFlags` caller has to preserve it.
- **Stop latching:** the two machines latch debugger stops differently, and PC-1600 `step()` misses watch hits.
- **Per-CPU branching:** `PC1600DebugTarget` branches on the CPU in about 17 methods.
- **Protocol code knows machine layout:** `DapSession` hardcodes bank and status-register layout.
- **Thread 1 hardcoded:** function breakpoints and attach symbols always go to thread 1.
- **Hit-path cost:** conditions are re-parsed on every hit, register lookups build full lists, and `WatchSet` scans linearly.
- **Test-only API:** the execution API has members only the tests use.

Decisions taken:
- **Event loop:** keep the nested `processEvents()` pump, but centralize it in one synchronous-operation service that the GUI and the debugger share. The macOS window-restoration opt-out stays.
- **Loaders:** one load pipeline with a per-caller options struct. Every caller keeps today's behaviour.

C++17 only. A new Core `.cpp` must be added to every hand-maintained source list:
- root `CMakeLists.txt` (Core list and `TEST_SOURCES`);
- `Qt6/CMakeLists.txt`;
- `tools/run_tests.sh`;
- the `tools/build_*.sh` scripts that compile it.

New types are header-only where that's natural. New test suites are registered in `main` in `Core/tests/lh5801_tests.cpp`.

## Workflow

First, commit this plan as `docs/Debugger-Restructuring-Plan.md`. Then implement the phases in order and commit each one to `dev-0.6.0` without stopping between them.

The gate for every phase:
```
cmake --build build && build/CoreTests && uv run tools/dap_smoke.py --app build/Qt6/Calc-U-1600.app/Contents/MacOS/Calc-U-1600
```

## Phases

### 1. Trim the DebugTarget execution API
- **Public primitives:** `runMachine()` and `stepMachine()` become public. Delete `run`, `step`, `runUntil`, `stepInstruction`, `runRaw` and `stepRaw` from `Core/Debug/DebugTarget.*`.
- **RunControl:** `RunControl.cpp:131,151` calls the primitives directly.
- **Test helpers:** new `Core/tests/DebugTargetTestSupport.hpp` with `runFrom`, `runUntil` and `stepInstruction`, the bodies copied over. Update `debug_target_tests.cpp` to use them.

### 2. Breakpoints independent of TRACE flags; lock-free BreakpointSet
- **BreakpointSet:** new header-only `Core/CPU/BreakpointSet.hpp`. It replaces the class at `TraceRing.hpp:70-101` with a 64K-bit map (`std::array<uint64_t,1024>`) and a plain `bool` hit flag, with no mutex.
- **CPUs:** LH5801 and SC7852 get `setBreakpointsEnabled()`/`breakpointsEnabled()`. `step()` tests that flag instead of `TRACE_BREAKPOINTS` (SC7852 keeps `&& !m_pendingPrefix`). `reset()` clears the hit and the skip-once state.
- **TRACE flag:** remove `TRACE_BREAKPOINTS` (`Core/TraceTypes.hpp`) and the masking in `PC1500Machine.cpp:235,251` and `PC1600Machine.cpp:609-610,625-626`.
- **Idle steps:** guard the idle-step consume (`PC1500Machine.cpp:141,200`; `PC1600Machine.cpp:274,324`) with `breakpointsEnabled()`.
- **Callers:** `setBreakpointFlag` in `MachineDebugTargets.cpp` becomes `cpu.setBreakpointsEnabled(on)`. Fix `tools/pc1500_cli.cpp:124`.
- **Tests:**
  - update `debug_target_tests.cpp` (lines 112/115/134/147/234/287) and `lh5801_tests.cpp:436`;
  - new: `BreakpointSet` edge cases at 0x0000 and 0xFFFF;
  - new: trace begin/end leaves the enable flag alone;
  - new: `reset()` clears the hit and skip-once state.

### 3. One machine-level debug-stop latch
- **Latch type:** new header-only `Core/CPU/DebugStop.hpp` with `DebugStop {kind: None|Breakpoint|Watch, cpu}` and `DebugStopLatch` (latch / consume / pending / clear).
- **Machines:** both get `consumeDebugStop()` and `setWatches(int cpu, WatchSet*)`.
  - `step()` and `runCycles()` latch breakpoint and watch stops the same way on both machines. PC-1600 `step()` latching watch hits is the missing piece today.
  - PC-1600 `runCycles` keeps its accounting order: a breakpoint stop consumes no time, a watch stop is counted after.
  - Machine resets clear the latch.
  - Rewrite the stale comment at `PC1600Machine.hpp:109-114`.
- **Removed:** `PC1500Machine::consumeBreakpointHit`/`m_breakpointStop`, the PC-1600 `DebugStop` enum, `PC1600DebugTarget::m_z80Watches`/`m_lh5803Watches`, and `DebugTarget::watchHitThread()` together with its fallback.
- **Targets:** both share one latch-to-Stop mapping.
- **Tests:**
  - PC-1600 `step()` latches LH5803 and Z-80 watch hits;
  - PC-1500 `step()` latches a watch hit;
  - `reset()` clears a pending stop;
  - the existing "stepping reports the watch" test passes without the fallback.

### 4. Per-CPU debug views
- **Views:** new `Core/Debug/CpuViews.*` with an abstract `CpuView`:
  - covers kind, name, registers, register read/write, pc, sp, history, retired, halted, pu/pv, apply/enable breakpoints and resume past a breakpoint;
  - `LhCpuView` (LH5801/LH5803) takes a `pupvChanged` callback: `memory().updatePUPV` on the PC-1500, `lh5803Memory().updatePUPV` on the PC-1600;
  - `Z80CpuView`;
  - keep the `halted` asymmetry: the LH580x also counts `poweredOff`, the Z-80 doesn't.
- **DebugTarget:** holds the views, and the per-CPU methods become plain forwards. `view(thread)` keeps today's mapping: 1 is the first CPU, anything else the last.
- **Remaining virtuals:** `busOwner`, `peek`, `poke`, `bankAt`, `runMachine`, `stepMachine`, `reset`, `attachWatches`.
- **Shared template:** `MachineDebugTarget<Machine>` implements run/step/reset/watches/latch mapping and the detaching destructor once. `PC1500DebugTarget` and `PC1600DebugTarget` derive from it.
- **LH5803 address mapping:** add `LH5803SharedMemory::toZ80Address()` and use it in its `debugPeek` and in `PC1600DebugTarget::poke`.
- **Tests:**
  - invalid-thread fallback;
  - an LH5803 PV register write reaches its memory;
  - the halted asymmetry;
  - the destructor leaves the CPUs clean.

### 5. Machine layout out of DapSession
- **Status register:** `Register` gets a `status` marker (`t` on the LH580x, `af` on the Z-80, history frames included). Add `debug::statusFlags(regs, &value)`.
- **Bank state:** `struct BankField {name, value}` and a virtual `bankState(thread)`:
  - the LH view reports PU/PV;
  - the PC-1600 Z-80 reports Page A–D.
- **DapSession:** delete `statusRegister` (`DapSession.cpp:41-47`). `bankVariables` (`:578-598`) just maps `bankState()`.
- **Tests:** `statusFlags` and `bankState` for both machines, live and history.

### 6. Cheaper lookups
- **Register reads:** `lhReadRegister`/`z80ReadRegister` (`CpuRegisters.cpp`) use a static name-to-getter table: registers, halves, `af2`-style aliases, then flags. They no longer build the whole list.
- **WatchSet** (`Core/CPU/WatchSet.hpp`):
  - lazily allocated bitmaps per space and read/write;
  - wrapped ranges (lo > hi) keep never matching;
  - the vector stays for `watches()`.
- **RunControl:** builds `bankMatch`, `codePeek` and `symbols` once as members and returns them by const reference.
- **Tests:** every register name reads the same value as before; WatchSet edge, space, access and wrap cases.

### 7. Compile conditions once
- **Compiled form:** `DebugExpression` gains `CompiledExpression::compile(text)` and `run(ctx)`, an RPN program. `evaluate(text, ctx)` becomes compile plus run for its other callers.
- **Error order:** a syntax error becomes a trailing `Fail` op, so the first error reported stays identical: a runtime error before a syntax error still wins, and `&&`/`||` still evaluate both sides.
- **BreakpointTable:**
  - `HitCondition::parse/met` and a `LogTemplate` replace the per-hit parsing. `hitConditionMet` and `interpolateLog` keep their signatures and output.
  - Armed and data entries carry the compiled condition, hit condition and log template, built where the entries are made.
  - A broken condition still stops at the hit with the same message.
- **Tests:**
  - a differential test against a test-only copy of today's parser (`Core/tests/LegacyExpression.hpp`), over a fixed corpus plus seeded random input;
  - hit-condition and log-template equivalence.

### 8. Per-thread symbols and function breakpoints
- **SourceMap:** add `findSymbol(name, &SymbolInfo{value, thread, key, binding})`, which skips stale bindings. `symbolValue()` wraps it, so it now skips stale bindings too (intended).
- **BreakpointTable:** `setFunctions()` and `reresolve()` drop the thread parameter and arm on the thread and bank key of the binding that defines the symbol, with an "Only while …" message when qualified. Update the callers at `DebugController.cpp:165` and `DapSession.cpp:703`.
- **Shared loader:** new `loadListingWithSymbols(listing, source, symbolFiles, &out, &warnings)` in `Core/Debug/Listing/`, used by `ProgramLoader`.
  - Symbols without a listing now bind as a symbols-only loaded binding (intended).
- **Attach `symbols`:** each entry may be a string (thread 1, as today) or `{path, cpu, bank, me, pu, pv}`, handled in `DapSession::prepare` (`:310-319`). Update the extension's `package.json` schema and `docs/Debugger.md`.
- **Tests:**
  - `findSymbol` returns the thread and key and skips stale bindings;
  - a function breakpoint on an LH5803 symbol stops on thread 2;
  - a symbols-only load binds.

### 9. One machine-code load pipeline
- **New pipeline:** new `Core/MachineCodeLoad.*` (namespace `machinecode`):
  - `LoadOptions`: target, `acceptLengthMismatch`, address/length overrides, `lh5803Space`, `check64K`, `SlotPolicy {BasicArea, BasicAreaOrInternal, Explicit}` with the slot, advice mode, entry;
  - `planLoad()`, which returns a `LoadError` code plus detail and the needs-address and default-address information;
  - `executeLoad()` with a writer callback;
  - per-machine `loadMachineCode(machine, bytes, options)` overloads in `PC1500MachineCodeLoader`/`PC1600MachineCodeLoader`.
- **Reused as building blocks:** `readFile`, `headerMismatch`, `plan`, `pc1600TargetFor`, `pc1600DefaultAddress`, `advice`, `pc1500UserRam`, `pc1600BasicAreas`, and the two writers.
- **Callers**, each keeping today's rules and error texts by mapping the `LoadError` codes:
  - GUI: `MainWindow::loadMachineCode` + `MachineCodeLoadDialog` + `PresetController::loadMachineCodeLive`;
  - debugger: `debug::loadProgram`;
  - presets: `PresetRunner.cpp:150-229`.
- **Tests:** decision tables for the GUI, debugger and preset configurations in `machine_code_file_tests.cpp`, plus the existing `presetloader_trace_tests` (which pins "'address' is required") and `pc1600_preset_tests`.

### 10. Synchronous-operation service (GUI side)
- **New service:** `Qt6/app/SyncOperations.*`, a QObject:
  - `run(title, op, afterLoad, error)` is `MainWindow::runSynchronousLoad` (`MainWindow.cpp:331-388`) moved over as-is: pacer suspend/resume with the clock seed, persist flush, wait cursor, 30 ms yield hook with filtered `processEvents`, progress popup after 500 ms, `discardAudio`;
  - nested calls do the outer work only once;
  - helpers `loadPreset`, `loadDefaultPreset(path, model)` and `resetToPrompt(allReset)`;
  - `busy()` and a `busyChanged(bool)` signal.
- **MainWindow:** uses the service everywhere. `isLoading()` becomes `busy()`. `onPresetArmed` (`:529`) pumps with `ExcludeUserInputEvents`.
- **DebugController:** takes `setSyncOperations()` and connects `busyChanged` to its now-private busy counter. Remove `setPresetLoader` and MainWindow's `setAppBusy` calls.
- **macOS:** the window-restoration opt-out is untouched.

### 11. Rebind the debug target once, on machine replacement
- **Hook:** `MachineController::wireNewMachine()` calls `m_debug->machineReplaced()`, pairing with `discardMachine()`'s `machineAboutToChange()`. `withMachine` becomes public.
- **Target creation:** new `makeDebugTarget(PC1500Machine&)` and `makeDebugTarget(PC1600Machine&)` overloads. `createTarget()` uses `withMachine` and always starts disarmed, which fixes today's arm-state inconsistency.
- **DebugController:** remove the three lazy `createTarget()` checks.
  - `runSlice` keeps the deferred resume and `onMachineReplaced()` behind a pending flag. Rebinding at wire time would mark every listing stale, because the machine hasn't booted yet.
  - `loadPreset` and `cleanStart` clear the flag, so they stay paused as today.

### 12. Debugger operations through the service
- **`cleanStart`:**
  - an explicit preset goes through the service's `loadPreset`;
  - the default preset goes through `loadDefaultPreset(path, currentModel)`, with the model check. A preset for another model now fails instead of switching models (intended);
  - with no preset, the service's `resetToPrompt(true)`.
- **`loadProgram` and `resetMachine`:** they run inside the service's `run()`. The entry breakpoint, `typeCommand` and resume stay after it.
- **Smoke tests:** extend `tools/dap_smoke.py` with:
  - an All Reset & Stop followed by continuing to a breakpoint;
  - a clean start without a default preset;
  - a preset load mid-session that re-verifies the breakpoints.

## Verification

- **Every phase:** the build, CoreTests (existing plus the new tests listed above) and `tools/dap_smoke.py`, with all four scenarios and the new ones from phase 12.
- **Behaviour regression, after phases 2–4 and 9:** plotter output identical for the PC-1500 CE-150 demo (1254 points), lissajou, and the PC-1600 lissajou. `examples/memtest_stock.pc1500` runs as before.
- **Speed:** the headless timing (`pc1600_cli` for 3 billion cycles, about 4.27 s before) doesn't get slower.
- **Manual check in VS Code** (`vscode/workspace` configurations), mainly after phases 10–12:
  - the memtest walkthrough: entry stop, conditional breakpoint, data breakpoint, memory view;
  - a PC-1600 LH5803 function breakpoint;
  - Build & Load repeated;
  - Settings ▸ Debugger ▸ Disconnect.
- **Docs:** update `docs/Debugger.md` (attach `symbols`, function breakpoints per CPU) and `docs/Debugger-Handoff.md` when done.
