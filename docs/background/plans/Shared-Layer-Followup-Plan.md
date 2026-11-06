# Shared layer follow-up: frontend-neutral seams

> **Status (2026-11-06): implemented** in phases 1–8 (594b570 … f5376d3); docs in phase 9. Differences from this plan:
> - **Phase 2:** the UI resync runs directly when `appBusyChanged(false)` fires (inside `AppSession::endOperation()`), not queued, so a caller sees a synced window when `SyncOperations::run()` returns (the screenshot runner relies on that). `FaceplateWidget::setModel()` now skips an unchanged model, since it runs after every operation.
> - **Phase 4:** `tick()` also throws away the audio of an operation slice (a browser's cooperative operation), not only turbo; without an attached sink it drops the audio.
> - **Phase 8:** `busChange()` is a member template in `MachineSession.hpp`; the rebuild test checks the CE-1600P but not the host drive (its ROM isn't in the test bundle).

## Context

The `/simplify` pass over the Shared-layer move (fe0ae19) left nine redesign
findings. Items 1–5 were introduced by the new layer itself; items 6–8 are
older Qt-era code that moved across unchanged; item 9 follows from item 2.
Goal: every rule a second (WebAssembly) frontend would otherwise have to copy
lives in Shared, and the new layer has one way to do each thing. One phase per
item group, each committed to `dev-0.8.0` on its own (autonomous phased
implementation); each phase passes `tools/run_tests.sh`,
`tools/build_wasm_check.sh` and the Qt build (`cmake --build build`).

Read `docs/background/Decisions.md` ("Shared layer", "Typing into the
machine", "Expansion bus") before each phase; settled questions go there.

## Phase 1 — Split `MachineSession::Listener` (new)

`AppSession::Platform` repeats 7 Listener hooks and AppSession forwards each.

- `Shared/MachineSession.hpp`: keep `Listener` for the frontend events
  (`romMissing`, `notice`, `machineAboutToChange`, `machineReplaced`,
  `modelSwitched`, `runDebuggerSlice`, `serialLinksRelinked`). Move
  `attachModulesToFreshMachine`, `flushFloppy`, `insertSelectedDisk` into a new
  `MachineSession::Bays` interface. Ctor: `MachineSession(Settings&, const
  AppPaths&, Listener&, Bays* bays = nullptr)` (tests without bays pass none).
- `Shared/AppSession.hpp`: `Platform : public MachineSession::Listener` adds
  only its own hooks (bay events, preset, attachFailed, busyChanged).
  AppSession passes `m_platform` as the Listener and privately implements
  `Bays`; delete the 7 forwarders.
- Adapt `Shared/tests/shared_machine_session_tests.cpp` (RecordingListener),
  `shared_bay_tests.cpp` (BayListener → Bays), `tools/wasm_smoke.cpp`,
  `Qt6/app/MachineController.hpp` (overrides unchanged, now of the base).
- Fix comments naming `MachineSession::Listener::flushFloppy()` etc.
  (FloppyBay.hpp, ModuleBay.hpp).

## Phase 2 — One busy state, one UI resync (new + item 9)

`SyncOperations::m_running`/`busyChanged` duplicate `AppSession::m_busy` /
`Platform::busyChanged` (never wired); MainWindow resyncs by hand after each
operation.

- `MachineController`: override `busyChanged(bool)` → `emit appBusyChanged(busy)`.
- `SyncOperations`: drop `m_running` (use `app().busy()` for the nested
  case) and its `busyChanged` signal; drop `setPresetResync` and the
  `afterLoad` parameter (its only use was the preset resync).
- `MainWindow`: connect `appBusyChanged(false)` once → queued
  `syncUiFromController()` + `drainPendingDrop()`; `syncUiFromController()`
  already covers menus, combos, peripherals. Remove the per-call-site
  `syncPeripherals()` / `refreshModuleCombos()` / `syncMachineMenuFrom…()` after
  `m_sync->run(...)` (moduleSelected, togglePeripheral, ROM/CE-1600P ROM
  selections, applyModelSelection). `onPresetArmed()` stays (mid-operation
  repaint via `Platform::presetArmed`).
- `DebugController`: connect `setAppBusy` to `MachineController::appBusyChanged`
  (it already defers via `QTimer::singleShot(0)`, so firing from
  `endOperation()` before the pacer resumes is safe).

## Phase 3 — Tape status once per frame (new regression)

- `AppSession::endOperation()` calls `frameDuties()` so the bays catch up
  after an operation (autosave tick, tape bay).
- `frameDuties()` keeps the `TapeDeck::Status` it read; add
  `const TapeDeck::Status& AppSession::tapeStatus() const`.
- `MainWindow::refreshViewsAfterAdvance()` uses `app().tapeStatus()`; drop
  `MachineController::tapeStatus()` if unused afterwards.

## Phase 4 — Audio policy into AppSession (new)

Frontend currently threads `turbo` from `tick()` into `AudioOutput::pump`, and
`EmulationPacer::runFor/runUntilPasteDone` call `frameDuties()` +
`discardAudio()` themselves.

- `AppSession::attachAudio(AudioSink&, AudioPump::Warn)` builds an owned
  `std::unique_ptr<AudioPump>`; `warmUpAudio()`. `tick()` pumps (discard =
  turbo) itself and returns `void`.
- `AppSession::runFor(seconds)` / `runUntilPasteDone(cap)`: wrap
  `shared::runEmulatedSeconds` / `shared::runUntilPasteDone` (Shared/Pacer.cpp)
  + `frameDuties()` + `discardAudio()`.
- `Qt6/app/AudioOutput`: only the QAudioSink (expose `shared::AudioSink&
  sink()`); MainWindow calls `attachAudio` and defers `warmUpAudio()` via
  `QTimer::singleShot(0)` as today. `EmulationPacer` loses its `AudioOutput*`.
  AudioOutput outlives the session (child of MainWindow, deleted after
  `m_controller`) — keep it that way.
- Test: `shared_app_session_tests.cpp` with a fake sink (turbo → nothing
  written; normal tick → BEEP samples reach the sink).

## Phase 5 — LCD indicators in Shared (new split)

`Shared/LcdLayout.hpp` holds only x fractions; captions, scale, style and
PC-1600 "I/II/III first match wins", PC-1500 KANA→"JPN", always-on battery dot
are in `Qt6/app/LcdWidget.cpp:paintPC1500/paintPC1600`.

- New `Shared/LcdLayout.cpp`: `struct Indicator { std::string text; double x;
  double scale = 1.0; enum Style { Text, Inverted, Dot } style; }` and
  `std::vector<Indicator> indicators(Model, const DisplayFrame&)`; x is the
  fraction of the indicator row width (PC-1500 interior / PC-1600 panel, via
  `p16StartX`). Uses `lcdAngleLegend` (Core/Display/LcdText.hpp).
- `LcdWidget` loops the list and draws by style; fonts, row geometry and the
  dot matrix stay.
- Test in `shared_geometry_tests.cpp`: per-model captions/positions for a few
  flag sets (KANA on PC-1500, I+II on PC-1600, BATT inverted, battery dot).
- Verify with a before/after screenshot (`--shots`) of both models.

## Phase 6 — Host key routing into MachineSession (old)

`MainWindow::typeResolved()` and the Shift-tap branch in `keyReleaseEvent`
decide per model between live queue, shifted tap, held press, enqueue, plus
"a live key interrupts a paste".

- `MachineSession`:
  - `bool hostKeyDown(const ResolvedKey&, bool physical)` — interrupts a
    paste, routes exactly as `typeResolved` does today; returns true when it
    pressed a key the frontend must release (physical PC-1600 plain key,
    PC-1500 repeat key).
  - `void hostKeyUp(const std::string& baseKey)` → `releaseKey`.
  - `void hostShiftTap()` — interrupt paste, live queue or `tapKey("shift")`.
- `MainWindow` keeps only the physical-key-id → baseKey map, the Shift-tap
  timer and `PC1500KeyboardMap::resolve`; `keyPressEvent`,
  `inputMethodEvent`, `keyReleaseEvent` call the three methods.
- The typing primitives stay public (tests and `typeCommand` use them); drop
  the now-unused MachineController forwarders.
- Tests in `shared_machine_session_tests.cpp`: one routing case per branch,
  both models (KBII, shifted, repeat key, committed char, paste interrupted).
- Decisions.md "Typing into the machine": note that routing lives in
  `MachineSession::hostKeyDown`.

## Phase 7 — Slot cards through MachineSession (old)

The PC-1500/PC-1600 "card in slot N" branch exists in
`ModuleBay::attachAllToFreshMachine`, `attachedModuleName`, `slotCard` and
`MachineSession::debugSlotCardImage`.

- `MachineSession`: `int slotCount() const` (PC-1500 1, PC-1600 2),
  `const ExpansionCard* slotCard(int) const`, `void attachSlotCard(int,
  std::unique_ptr<ExpansionCard>)`, `void detachSlots()`.
  `debugSlotCardImage` keeps its `debugInspect` lock but uses one private
  per-machine helper.
- `ModuleBay`: loop `1..slotCount()`; `attachedModuleName` =
  `slotCard(slot)->moduleName()` (identical to `PC1600Memory::slotModuleName`);
  no more `pc1500()`/`pc1600()` includes in ModuleBay.cpp.
- Existing `shared_bay_tests` cover both models; add a PC-1500 slot-2 = null case.

## Phase 8 — Peripherals snapshot + one bus-change path (old)

- `switchModelJob`: snapshot `{ce150, ce1600p, ce158, hostDriveDir}` before
  discarding; after the model-specific build, one common tail:
  `attachModulesToFreshMachine` then `restorePeripherals(snapshot)` in today's
  order (CE-150, CE-1600P, CE-158, host drive). `attachCE1600P` /
  `attachHostDrive` are already no-ops off a PC-1600, so the PC-1500 branch
  behaves as before. Rename `keepPlotter` → `keepPeripherals` (11 uses).
- Floppy flush: one private `busChange(fn)` helper that runs
  `flushFloppyBeforeDetach()` then `fn`, used by `attachCE150`, `attachCE158`,
  `detachCE1600P` and the rebuild; no bare flush calls left.
- Test: rebuild with CE-150 + CE-158 on PC-1500, CE-1600P + host drive on
  PC-1600 (`setPC1600RomVersion`), all still attached; floppy autosave
  written before a CE-150 takes the bus.

## Phase 9 — Docs

- This plan → `docs/background/plans/Shared-Layer-Followup-Plan.md` + row in
  `docs/background/README.md`.
- `docs/developer/Shared-Architecture.md`: Listener/Bays split, audio in
  AppSession, host key routing, slot-card API.
- Decisions.md "Shared layer": settled items (busy state lives in AppSession
  only; UI resync on busy=false; tick() owns audio discard).
- Remove anything from TODO.md's "not in Shared yet" list that this covers.

## Verification

- Per phase: `tools/run_tests.sh`, `tools/build_wasm_check.sh`,
  `cmake --build build` (Qt app, no warnings).
- After phases 2, 4, 5, 6: launch the app (`-ApplePersistenceIgnoreState
  YES`, quit via `calcu1600/quit`) and check by hand: model/ROM/module
  switches update the control bar and menus; BEEP audible, silent in turbo;
  LCD indicators on both models (screenshots before/after); typing,
  Shift tap, cursor auto-repeat on PC-1500, accented KBII chars on PC-1600,
  paste interrupted by a key.
- Phase 8: toggle CE-150/CE-1600P/CE-158/host drive, switch ROM, confirm all
  survive; floppy writes survive attaching a CE-150.
