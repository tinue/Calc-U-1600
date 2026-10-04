# Tape as control-bar media (like cards and floppies)

> **Status (2026-10-04): implemented.** Differences from the plan below:
> the scenario shot runner gained an `accept` step (and `select:` now also
> emits `activated`), and the tape combo reacts to `activated`, so picking
> the recording tape it already shows puts it in to play.


## Context

The cassette recorder works (CLOAD/CSAVE, verified against real PC-1500 and
PC-1600 recordings), but it's armed through file dialogs (File ▸ Tape ▸
Play… / Record… / Eject, also behind the control bar's Tape button). That's
unlike the other media: memory cards and floppies are picked on the control
bar from a fixed folder, with a one-line Name & Save. The dialog route also
caused the focus problem fixed in 5110f1a. The user wants tapes to follow
the media pattern:

- A fixed **tape folder** with a row in Settings ▸ Storage; default
  `<save folder>/Tapes` (e.g. `~/Calc-U-1600/Tapes`).
- The bay starts **empty**. The **drop-down** lists the folder's WAVs;
  picking one loads it **for playing** (CLOAD). A **Save** button asks for
  a name (the floppy Name & Save one-liner) and loads a blank tape **for
  recording** (CSAVE). An existing name asks, then overwrites.
- **The role stays fixed**: a tape picked in the drop-down is play-only;
  CSAVE never writes to it. The ▶/● counter already shows the role.
- **Auto-save on motor-off**: when the remote relay stops the motor during
  a recording, the WAV is written (whole). Eject only un-arms. While the
  tape stays armed, each further CSAVE appends and is saved again at its
  motor-off.
- **File ▸ Tape is removed**, with its file dialogs. A future "File ▸ Load
  WAV" fast load (needs WAV support in sde) is a separate feature.

Tape behaviour otherwise stays as is: playback continues from where the
motor stopped; a machine rebuild ejects (nothing is lost, since recordings
are saved at each motor-off).

## Core: `Core/Tape/TapeDeck.hpp`

- `setMotor(false)` while recording with a path and unsaved samples →
  write the whole recording (`writeWavMono16`). Keep a "saved up to
  sample N" mark so a second motor-off without new data doesn't rewrite.
- `eject()` → writes only if something is unsaved (e.g. Eject while the
  motor still runs mid-CSAVE), then un-arms. Return value/`error` unchanged.
- A failed auto-save can't return an error (it happens inside emulation):
  store it as `lastError` in `Status`, cleared when read
  (`takeLastError()`), so the GUI can show it once.
- Machines: `PC1500Machine` / `PC1600Machine` `tapeStatus()` already lock;
  add a locked `tapeTakeLastError()` beside `tapeEject()`.
- CLIs (`--tape-out`) keep working; their final eject now usually finds
  nothing unsaved.

## GUI

- **`Qt6/app/AppSettings.hpp` / `AppPaths.{hpp,cpp}`:** `tapeDirOverride`
  setting, `AppPaths::tapeDir()` = override or `instanceDir()/Tapes`
  (created on demand), following `instanceDir()` / `instanceDirOverride()`.
  Remove `OpenFolder::Tape` and its General row.
- **`Qt6/app/SettingsDialog.cpp`:** Storage gets a "Tapes:" path row (same
  `PathRowSpec` as "Battery-card saves").
- **New `Qt6/app/TapeManager.{hpp,cpp}`** (pattern: `FloppyDiskManager`):
  - `tapeNames()`: the `*.wav` in `tapeDir()`, sorted, without extension.
  - `selectForPlay(name)` → `controller->tapePlay(path)`; `""` →
    `tapeEject()`.
  - `recordNew(name, error)`: validates the name (same rules as Name &
    Save: non-empty, file-name safe), returns "exists" so MainWindow can
    ask; then `tapeRecord(path)`.
  - Holds the current tape name and role for the combo; emits
    `errorMessage` for write failures (polled `tapeTakeLastError()`).
  - `MachineController::tapePlay/tapeRecord/tapeEject/tapeStatus` stay;
    `discardMachine()` keeps ejecting (now cheap).
- **`Qt6/app/ControlBar.{hpp,cpp}`:** replace the Tape menu button with
  `Tape: [combo] [save button] ▶ 0:12 ●` (objectNames `controlbar.tape`,
  `controlbar.tape.save`, `.counter`, `.lamp`), all `NoFocus`.
  `setTapeCombo(names, selected)` via the existing `fillPicker()`
  ("–empty–" first). While recording, the combo shows the recording tape's
  name (added to the list if its file doesn't exist yet). Remove
  `setTapeActions()`.
- **`Qt6/app/MainWindow.{hpp,cpp}`:** wire combo → `selectForPlay`, save
  button → `QInputDialog::getText` (as `floppyNameAndSaveRequested`),
  "exists" → `QMessageBox::question` to replace. Refresh the combo after
  each motor-off save and on `syncPeripherals()`. Remove File ▸ Tape,
  `playTape/recordTape/ejectTape`, `ReturnFocusToCalculator`,
  `syncTapeActions()`; `closeEvent` still ejects.

## Docs and screenshots

- `docs/User-Guide.md` §4 "Cassette tape": rewrite for the bar (pick a
  tape to play, Save to record, auto-save at motor-off, append while armed,
  fixed roles); Settings §9.1: "Tapes" under Storage instead of General.
- `docs/developer/screenshots/guide/04-basic.shots.yaml`: the shot run uses
  an empty save folder, so record first, then play: click
  `controlbar.tape.save`, `enter-text` a name, `type: CSAVE`, `run`, then
  `select: controlbar.tape = <name>`, `type: CLOAD`, capture. Regenerate
  04 and 09 (Settings) images. Update the README's widget-name table.
- `docs/background/Decisions.md`: tapes are fixed-role media; auto-save at
  motor-off; eject only un-arms.
- CHANGELOG entry adjusted (no File ▸ Tape).

## Verification

- `tools/run_tests.sh`: `tape_deck_tests` gains motor-off auto-save
  (file written at motor-off, appended and rewritten at the next one, no
  rewrite without new data, eject writes only unsaved data, write error
  reported once); ROM tape tests unchanged.
- `tools/build_app.sh` builds; both tape matrices still pass
  (`dev/tape-matrix/run.py pc1500|pc1600`).
- GUI via the 04-basic scenario (record → auto-saved file appears in the
  combo → select → CLOAD shows the program) and by hand: Save over an
  existing name asks; picking a tape while recording un-arms it (file
  already saved); Settings ▸ Storage "Tapes" changes the list.
- Commit to dev-0.8.0 in steps (Core, GUI, docs).
