# Calc-U-1600

Calc-U-1600 is an emulator for the Sharp PC-1500, PC-1500A, and PC-1600
pocket computers, built as a Qt6 desktop app (macOS / Linux / Windows).
A preview of a web version runs in the browser at
<https://tinue.github.io/1600>.

## Quick Start

1. From the [Releases page](https://github.com/tinue/Calc-U-1600/releases),
   download the package for your platform, along with
   `Calc-U-1600-examples.zip` — the sample presets and BASIC programs
   used below.
2. Launch the app and pick one of the presets under **File ▸ Examples**:
   each builds a machine and runs a program on it.
3. Open **Settings…** (Cmd-, / Ctrl-,), and point **Samples folder** at
   the unzipped `examples/` folder from step 1.
4. Pick a model (PC-1500, PC-1500A, or PC-1600) from the control bar, and
   attach a memory module and/or a plotter if you'd like to try those.
5. **File ▸ Load BASIC Program…**, pick a `.bas` file from `examples/`
   (e.g. `plotter/lissajou-1600.bas` on a PC-1600), and `RUN` it.
   `examples/README.md` lists what else is in there.

## Status

This is a **beta release**. The emulation itself is complete and
working: all three models run their real firmware, not a simulated
subset. Future releases will focus on the surroundings around that core
— things like the debugger and the Inspector — rather than the emulation
itself. What changed in each release is in [CHANGELOG.md](CHANGELOG.md).

## Web version (preview)

<https://tinue.github.io/1600> runs the same emulator in the browser,
compiled to WebAssembly, with the desktop app's faceplate, menus, control
bar and panels. It is a preview, made for tablets and desktop browsers; a
phone works, and in landscape it shows the calculator alone. The browser
can install it as an app, which then runs offline.

- **File ▸ Load Preset** has the bundled examples; **Upload Preset…** loads
  your own.
- Cards, disks and tapes you save stay in the browser.
  **File ▸ Import Card, Disk or Tape…** brings files in, **File ▸ Saved
  Media…** downloads or deletes them. Only one tab saves; a second tab
  says so.
- A link can choose the model (0 PC-1500, 1 PC-1500A, 2 PC-1600) and a
  bundled preset to start with:
  `https://tinue.github.io/1600/calc/?model=2&preset=plotter/lissajou-1600.pc1600`.
- Not in the browser: the debugger, the serial ports, the host drive
  `S3:`, and the Inspector's TRACE and LOG.

## Documentation

**Using the emulator**

- [User Guide](docs/User-Guide.md) — start here: models, keyboard,
  plotters, loading BASIC and machine-code programs, memory modules and
  floppy disks, COM ports and presets. With screenshots and diagrams.
- [Keyboard Mapping](docs/Keyboard-Mapping.md) — every host key,
  including the PC-1600's accented characters.
- [Host drive](docs/PC1600-Host-Drive.md) — a folder on your computer as
  PC-1600 drive `S3:`.
- [Examples](examples/README.md) — what each sample preset and program does.

**Writing programs for the PC-1500 / PC-1600**

- [Debugger](docs/Debugger.md) — debugging machine code (and the ROMs)
  from VS Code: your own programs, programs found online, ROM extensions,
  firmware analysis.
- [VS Code extension](vscode/calcu1600-debug/README.md) — what the
  Calc-U-1600 Debugger extension provides.
- [PC-1500 keyword modules](docs/PC1500-Keyword-Modules.md) — how a ROM
  extension adds BASIC commands to the PC-1500.

**Custom memory modules**

- [Memory Card Definition — Specification](docs/Memory-Card-Definition-Spec.md)
  — what a `.card.yaml` describes, and why.
- [Memory Card Definition — Format](docs/Memory-Card-Definition-Format.md)
  — every key of a `.card.yaml` file.

## Serial port

The emulated PC-1600's RS-232C port appears on your computer as a serial
port file, `calcu1600-rs232c.serial` (the CE-158's as
`calcu1600-ce158.serial`) — **macOS and Linux only** for now. Point any serial program at it, for example
[SharpDataExchange](https://github.com/tinue/SharpDataExchange) to copy
programs to and from the calculator. The User Guide's
[COM ports](docs/User-Guide.md#7-com-ports) chapter has the settings.

## License

Copyright (C) 2026 Martin Erzberger. Calc-U-1600 is free software,
licensed under the [GNU General Public License, version 3](LICENSE)
(GPLv3); see [NOTICE](NOTICE) for the full copyright notice.

It's built on [Qt6](https://www.qt.io/), used under the LGPLv3 -- see
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for Qt and other
third-party components (including the `roms/` ROM images, which are
third-party copyrighted material not covered by this project's license).

---

Building Calc-U-1600 from source, or working on the emulator itself? Start
at [docs/developer/README.md](docs/developer/README.md).
