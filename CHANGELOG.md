# Changelog

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
