# Changelog

## [0.3.0] - 2026-09-19

- **PC-1600F** Added the CE-1600F pocket floppy drive for the
  PC-1600. It attaches with the CE-1600P plotter/printer (no separate
  switch) and works like the real one: `INIT"X:"` formats a blank disk,
  and `SAVE`, `LOAD`, `FILES` and `DSKF` work on it. Disks are
  double-sided (64 KB per side); a control-bar button flips the disk to
  the other side, a green lamp shows when the drive motor runs, and
  drive timing follows the Service Manual.
- **Floppy disk** Two templates are available: A formatted floppy,
  and a blank one. Select one in the drop down to insert into the
  drive. Any changes you make on these templates are in-memory only.
  If you want to persist, click the "save" button and give the floppy
  a name.
- **Presets** Settings has a default preset per model (PC-1500,
  PC-1500A, PC-1600), applied at startup and whenever that model is
  selected. Ready-made ones are in `examples/startup/`; the PC-1600 one
  adds a CE-1600M and CE-1601M, mounts a floppy, sets up the RAM
  disk and COM1.
- **Loading** Long preset loads show a "Loading…" popup and the window
  keeps updating, instead of freezing.
- **Settings** The Settings dialog is reorganized into titled sections
  (General, Default presets, Storage, Tracing, Serial port).
- **Control bar** Common controls (Reset, Settings, model/ROM, memory
  slots) stay left-aligned, so switching models no longer shifts them.
  The redundant "Load Preset…" button is gone; use the File menu.
- **Build** Windows ARM64 is now built natively and gains BASIC preset
  loading; BASIC tokenizer updated to SharpDataExchange 0.2.1.

## [0.2.0] - 2026-09-18

- **Platforms** Added a native Windows ARM64 build, alongside the
  existing Windows x86_64, Linux, and macOS builds.
- **Keyboard** PC-1500/1500A live typing is now buffered, so typing
  quickly no longer drops keystrokes. Cursor keys (Left/Right/Up/Down)
  still pass straight through, so the ROM's own auto-repeat still works
  when held.
- **Startup** A missing or corrupted bundled ROM now shows a dialog
  naming the file and where it looked, instead of silently crashing.
- **Loading** Support loading segmented PC-1600 BASIC programs.
- **Loading** File ▸ Load Preset… no longer asks for confirmation before
  resetting the machine — picking a preset loads and resets immediately.
- **Loading** File ▸ Load BASIC Program… now behaves like a real LOAD:
  it no longer resets the machine, switches RUN/PRO mode, or types NEW0.
  It loads into whatever BASIC program area is currently live, so you can
  prepare the machine yourself first (fit a memory card, run `NEW`,
  attach a plotter) exactly as you would on real hardware.
- **Display** Press and hold anywhere on the LCD to fast-forward the
  emulation at full, unthrottled speed; release to return to normal
  speed.
- **Windows installer** Fixed a "missing MSVCP140.dll" failure on a
  clean machine by running the bundled Visual C++ redistributable
  during install.

## [0.1.0] - 2026-09-15
First public release. Calc-U-1600 is an emulator for the Sharp PC-1500,
PC-1500A, and PC-1600 pocket computers, built as a Qt6 desktop app for
macOS, Linux, and Windows.

- **Machines** All three models boot to BASIC, with keyboard, dot-matrix
  display, real-time clock, expansion-card, and serial-port support.
- **Plotters** CE-150 (PC-1500/1500A/1600) and CE-1600P (PC-1600) pen
  plotters, rendered on a virtual paper roll/sheet you can copy or export
  as an image.
- **Memory modules** Battery-backed and anonymous expansion-card modules
  per model, selectable and nameable from the control bar; state persists
  across sessions for named modules.
- **Presets** `.pc1500`/`.pc1500a`/`.pc1600` preset files describe a full
  machine configuration (model, ROM revision, modules, plotter, a resident
  program) and load it in one step via File ▸ Load Preset….
- **BASIC programs** File ▸ Load BASIC Program… tokenizes and loads a
  plain `.bas` listing directly into the running machine, without needing
  a preset wrapper.
- **Serial port** The PC-1600's RS-232C port is always live, backed by a
  host pseudo-terminal any serial application can connect to.
- **Native menu bar** File, Machine, and Help menus alongside the existing
  control bar, including an About dialog with version/build information.
