# Shared layer between Core and Qt6 (Calc-U-59 pattern), WebAssembly-ready

> **Status (2026-11-06): implemented** in phases 0–5 (6eaebd7 … f44da77). Architecture: docs/developer/Shared-Architecture.md. The implementation differs from this plan in these places:
> - **Order:** the tape/module/floppy bays moved after `MachineSession` (they act on the machine), not with settings and paths.
> - **Jobs are C++20 coroutines** (Core/Job.hpp), chosen over hand-written C++17 state machines: the typers, boot, preset runner and loaders read as before, with `co_await job::run(machine, n)` for each run. The project is C++20.
> - **`AppSession`** became the composition the plan's `OperationQueue` and `PresetSession` were to be: it runs presets, resets, model/module/ROM selections, peripheral toggles and the host-drive mount as jobs (`start()` + `tick()` for a browser, `runNow()` for Qt). `PresetController` and `PlotterController` are gone.
> - **No `Ce158PrinterText`:** the printer's bytes→text was already in Core (`ce158PrintableText`).
> - **Not done, in TODO.md:** CLIs running presets through Shared (and the duplicated `drainWav` hook), the dialog-led `MainWindow` flows (multi-file tape pick, machine-code address/advice, drop dispatch), the DAP debugger (planned for later), running the CI `wasm-check` job.

## Context

`Qt6/app/` (~10k lines) mixes GUI code with frontend-neutral logic: model switching, power cycles, pacing, preset orchestration, catalogs for memory modules, floppies and tapes, settings keys, paper geometry and key maps. A future WebAssembly GUI would have to duplicate all of it. Calc-U-59 (`~/Development/public/Calc-U-59`) solved the same problem with three layers:

- `Core/` is the hardware.
- `Shared/` is C++17, standard library only, and runs without threads.
- The frontends are thin and only translate types.

This plan applies that layering here. Choices already settled with the user:

- A compile-only Emscripten check of Core+Shared comes with this work.
- The DAP debugger (`Qt6/app/debug/`) stays in Qt6 for now and is listed as a later phase.
- Long synchronous operations (boot, presets, loads, power cycles) become **resumable jobs** pumped by `tick(now)`, because a browser main thread cannot block for seconds.

Facts the design relies on:

- **The app is single-threaded.** Emulation runs on the GUI thread through `EmulationPacer`'s 16 ms QTimer. Sync operations block and pump `processEvents` through a yield hook. So, unlike Calc-U-59, **no `Threading::Threads` mode is needed**: Shared is single-threaded and cooperative only, and the Qt and web frontends both just call `tick(now)`.
- **Core has no Qt includes.** The WASM blockers are:
  - `Core/Serial/PtySerialLink.cpp`: Emscripten defines `__unix__`, so its guard lets it compile.
  - `libsharpdx`: a prebuilt arm64 `.a`. Its Rust source is `../SharpDataExchange`; with `--no-default-features` it depends only on `anyhow`.
  - Blocking `runCycles` chains in `Core/Preset/PresetRunner`, the BASIC typers and `runBootToPrompt`.
- **Core sources are listed by hand** in the root `CMakeLists.txt`, `Qt6/CMakeLists.txt`, `tools/build_*.sh` and `tools/run_tests.sh`.

## Layering rules (go into CLAUDE.md + docs/developer/Shared-Architecture.md)

- `Shared/` holds everything that is neither emulation nor drawing. It is C++17 and standard-library-only, and never includes Qt, Cocoa or Emscripten headers.
- Shared never works out where files live. It takes `std::filesystem::path` values, or a `Platform` / `ResourceReader` interface the frontend implements: bundled resources, instance directory, settings store, message and error sink, audio sink.
- Settings key names and defaults live in `Shared/SettingsKeys.hpp`. Storage stays per frontend (QSettings or localStorage) behind a `SettingsStore` interface.
- Frontends hear about changes Calc-U-59 style: they call `AppSession::tick(nowNs)` about every 16 ms, then get `Platform::changed(mask)` with bits such as `Display | Plotter | Ce158 | Modules | Floppy | Tape | Busy | Model`, and pull the data through getters. Signals become callbacks or mask bits; `QTimer::singleShot` choreography becomes frame- or cycle-counted steps.
- `Qt6/app/` adapters only translate types (QString↔std::string, QImage from pixel buffers, and so on).
- Namespace `shared` (Core uses lowercase per-area namespaces).

## Phases (each: build, `tools/run_tests.sh`, Qt app smoke test, commit on dev-0.8.0)

### Phase 0: Build plumbing
- Add `calcu_core` and `calcu_shared` static libraries in a new `cmake/CalcUSources.cmake`. Include it from the root `CMakeLists.txt` and from `Qt6/CMakeLists.txt`, which must still configure standalone (`-S Qt6`). Remove the duplicated Core lists from both CMake files; the CLI and probe targets link `calcu_core`.
- Leave `tools/build_*.sh` and `run_tests.sh` as they are unless they can call CMake. Document in a comment that CMake is the source list.
- Create `Shared/` and `Core/tests/shared_*_tests.cpp`, added to `CoreTests`. Existing harness style; tests are maybe better in a new `Shared/tests/`, following the `Core/tests` layout.
- Guard `PtySerialLink.cpp` with `&& !defined(__EMSCRIPTEN__)`; `SerialLink` stays the interface.

### Phase 1: Pure moves (no behaviour change)
- `Qt6/app/keylayout/*` → `Shared/KeyLayout/` (already standard-library-only).
- `PaperGeometry.hpp` → `Shared/PaperGeometry.hpp`, using its own `Point` struct in place of `QPointF`.
- Export caps, pen colours and the stroke builder from `PlotterPaperWidget` → `Shared/PlotterPaper.{hpp,cpp}`, which produces per-colour polylines. The widget only paints them.
- LCD layout constants and status-indicator x-fractions from `LcdWidget` → `Shared/LcdLayout.hpp`; `FaceplateWidget::hitTest` scaling → `Shared/FaceplateGeometry.hpp`.
- CE-158 printer bytes→text from `Ce158PrinterWidget` → `Shared/Ce158PrinterText`.
- Key tables in `PC1500KeyboardMap` → `Shared/HostKeyMap`, with a neutral key enum (`HostKey`, close to DOM `KeyboardEvent.code`) plus modifiers. The Qt adapter maps `Qt::Key` → `HostKey`.

### Phase 2: Settings, paths, catalogs
- `AppSettings.hpp` → `Shared/SettingsKeys.hpp` for keys, defaults and resolvers such as `openStartDir` fallback, `startup/modelPreference`, DAP port range, `maxFileSizeMB`. Add `Shared/SettingsStore.hpp`, an interface; Qt implements it over QSettings.
- From `AppPaths`, move `instanceDir` resolution, `displayPath`, `isUnderDir` and the `planNamedSave` refusal mapping → `Shared/AppPaths` (std::filesystem). `bundledResourcesDir` and the atomic `QSaveFile` write stay in Qt, behind `Platform::writeFileAtomic`.
- `TapeManager` → `Shared/TapeBay`.
- `MemoryModuleManager` → `Shared/ModuleBay`, and `FloppyDiskManager` → `Shared/FloppyBay`. This covers the lists (bundled first, with shadowing), classify, attach, `saveAs`, revision-gated autosave and side/motor. The 500 ms QTimer debounce becomes a deadline checked in `tick`. The Qt classes become thin QObject wrappers that forward and re-emit.

### Phase 3: Machine session and pacing
- `MachineController` → `Shared/MachineSession`. It owns the PC1500 or PC1600 machine and covers: `switchModel`, ROM load with the old→new fallback (`makePC1600WithRomFallback`, now reused by `pc1600_cli`), re-attaching peripherals and the host drive, paste and live typing (`enqueueTyped`, `pasteOnFrame`), `currentDisplay`, tape/plotter/CE-158/trace/inspector pass-throughs and `modelSettingsKey`.
  - QMessageBox and `std::exit` → `Platform::reportError` / `fatal`.
  - `tapKey` / `tapShiftedKey` QTimers → cycle-counted key steps.
- `PlotterController` → part of `MachineSession` (peripheral toggles).
- `EmulationPacer` math → `Shared/Pacer`. `tick(nowNs)` covers elapsed×Hz plus carry, the 0.1 s cap and the turbo budget. Turbo is a fixed time slice per tick in place of a `processEvents` busy loop.
  - Fix the drift against `Decisions.md` § "Absolute-deadline emulation pacing". Either implement absolute deadlines (`start + n·batch`) as the decision says, or update the entry, whichever matches the measured behaviour. Check RTC/TIME accuracy as in memory `project_pc1600_rtc_power_off_lag_fixed`.
- `AudioOutput` backlog policy (prime, trim, idle stop) → `Shared/AudioBacklog`, independent of the sink. QAudioSink stays in Qt.
- `Shared/AppSession` ties these together and pulls the sequences out of `MainWindow`:
  - `applyModelSelection` order, `refreshViewsAfterAdvance` per-frame duties, `openDroppedFile` dispatch, `readProgramFile` (multi-file tape pick → `Platform::choose`), and the machine-code load plan/advice flow.
  - `MainWindow` keeps menus, dialogs and widgets.

### Phase 4: Resumable jobs (cooperative operations)
- `Shared/Job.hpp` (or `Core/Job.hpp`, because Core's preset code needs it): `struct Job { virtual JobStatus step(uint64_t cycleBudget) = 0; }`, plus `runToCompletion(Job&)` for the CLIs and tests.
- Turn the blocking primitives into small state machines, in Core, keeping their names:
  - `runCycles`, `waitUntilBasicIdle`, `presetTapOn` / key holds, `typeLine`, the BASIC typers (`PC1500BasicTyper`, `PC1600BasicTyper`), `runBootToPrompt`, power-cycle frames.
  - `runPresetSections` becomes an interpreter over the step list with a current-step job.
  - `applyPC1500Preset` / `applyPC1600Preset` become arm → boot job → sections job. The existing `onArmed` and `onBooted` callbacks fire at the same points.
- **One implementation**: the blocking API stays as a thin wrapper, `runToCompletion(makePresetJob(...))`, so the CLIs, probes and every existing test keep calling the same functions.
- `SyncOperations` / `PresetController` → `Shared/OperationQueue`.
  - At most one active job, with the Busy mask bit; flush modules and floppies before, re-seed the pacer after.
  - `AppSession::tick` gives the active job a time-sliced cycle budget (about 10 ms of host time at full speed, as in Calc-U-59 `kFullSpeedSlice`).
  - Remove the yield hook and `processEvents` from the GUI path. Qt keeps its progress dialog, driven by the Busy bit plus a progress value.
- Unify the CLI preset env: `Shared/PresetSession` builds `PresetEnv` and the `saveas:` dispatch (`savePresetMedia` / `pc1600PresetMedia` / the bays). `tools/pc1500_cli.cpp` and `tools/pc1600_cli.cpp` use it, which removes their copies of the `drainWav` hook.
- **Equivalence check (required before the commit):** for every preset in `examples/`, the bundled presets and `tools/check_presets.sh`, the cooperative run with random budgets must produce the same final cycle count, RAM hash and LCD text as the blocking wrapper. Add this as a CoreTests case over a representative subset (with a fixed seed); run the full sweep once in `dev/job-equivalence/` (memory: one-time sweeps in their own directory). Re-run the loader matrix (`dev/`) for PC-1500 and PC-1600.

### Phase 5: WASM compile check
- Add `tools/build_wasm_check.sh`: `emcmake cmake -S . -B build-wasm -DCALCU_WASM_CHECK=ON` builds only `calcu_core` + `calcu_shared`, with `-std=c++17 -fexceptions`, no `-pthread`.
- Add a tiny `tools/wasm_smoke.cpp` to the check. It runs one PC-1600 and one PC-1500 preset through `AppSession` with `node` and fake ROM/resource readers (ROMs come from `roms/` through NODERAWFS). The check then does more than compile; skip it if no ROMs are present.
- `tools/build_sharpdx_wasm.sh` builds `../SharpDataExchange` for `wasm32-unknown-emscripten` with `--no-default-features`, giving `Core/Basic/vendor/sharpdx/libsharpdx-wasm.a`. It is committed like the other prebuilt libraries and fetched in CI.
- Add a `wasm-check` job to `.github/workflows/build.yml` (emsdk setup, Rust target, build, node smoke test).

### Later (not in this plan; TODO.md rows)
- The DAP debugger to Shared: `DapSession`, `DebugController` and `DapServer` framing. This needs a JSON value type first, such as vendored nlohmann/json.
- The web GUI itself: `Web/bindings.cpp` with embind, a worker, and a FrontendParity table like Calc-U-59's `reference/FrontendParity.md`.

## Docs
- New `docs/developer/Shared-Architecture.md` with the layers, Platform interface, tick/change-mask model, job model and WASM constraints. Add a row in `docs/developer/README.md`, update `docs/developer/Building.md` (libraries, wasm check) and update CLAUDE.md with the layering rules.
- Add a `Decisions.md` entry: "Shared is single-threaded/cooperative only (no Threads mode, unlike Calc-U-59)", plus the pacing entry fixed in Phase 3.
- When finished, copy this plan to `docs/background/plans/Shared-Layer-Plan.md` with a row in `docs/background/README.md`.

## Verification
- After each phase: `tools/run_tests.sh` green, the `CoreTests` CTest target, `cmake -S Qt6` standalone build, and `build_and_run.sh`.
- Smoke-test the Qt app with `-ApplePersistenceIgnoreState YES` and quit through `calcu1600/quit`. Check:
  - switch the model to PC-1500 and back
  - load a default preset (check `--lcd-text -`)
  - paste BASIC, run a CE-150 / CE-1600P plot
  - insert a module and confirm it autosaves; select a floppy; play a tape; make sure BEEP audio sounds
- Phase 3: TIME$ drift check over 60 s against the host clock.
- Phase 4: the equivalence test, plus `tools/check_presets.sh` and the loader matrix.
- Phase 5: `tools/build_wasm_check.sh` plus the node smoke test locally, and the CI `wasm-check` job green.
- `tools/dap_smoke.py`, so the debugger keeps working on top of the moved session.
