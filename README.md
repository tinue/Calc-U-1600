# Calc-U-1600

Calc-U-1600 is an emulator for the Sharp PC-1500, PC-1500A, and PC-1600
pocket computers, built as a Qt6 desktop app (macOS / Linux / Windows).

## Quick Start

1. From the [Releases page](https://github.com/tinue/Calc-U-1600/releases),
   download the package for your platform, along with
   `Calc-U-1600-examples.zip` — the sample presets and BASIC programs
   used below.
2. Launch the app, open **Settings…** (Cmd-, / Ctrl-,), and point **Default samples
   folder** at the unzipped `examples/` folder from step 1.
3. Pick a model (PC-1500, PC-1500A, or PC-1600) from the control bar, and
   attach a memory module and/or a plotter if you'd like to try those.
4. **File ▸ Load BASIC Program…**, pick a `.bas` file from `examples/`
   (e.g. `plotter/lissajou-1600.bas` on a PC-1600), and `RUN` it.
   `examples/README.md` lists what else is in there.

## Status

This is a **beta release**. The emulation itself is complete and
working: all three models run their real firmware, not a simulated
subset. Future releases will focus on the surroundings around that core
— things like the debug panel — rather than the emulation itself. See
[TODO.md](TODO.md) for known issues and open work.

## Documentation

- [docs/User-Guide.md](docs/User-Guide.md) — using the emulator, from the
  basics (models, keyboard, plotters) to loading BASIC and machine-code
  programs, memory modules and floppy disks, COM ports and presets. With
  screenshots and diagrams.
- [docs/Building.md](docs/Building.md) — building from source
  (prerequisites, ROMs, per-platform build steps).

## Serial port

The emulated PC-1600's RS-232C port (and the CE-158's) is always live and
backed by a host pseudo-terminal — **macOS and Linux only** (there's no
Windows equivalent yet). Point any serial application at the stable
`calcu1600.serial` symlink in the folder set under **Settings ▸ Serial
ports**. For the calculator-side settings and how to exchange programs
with [SharpDataExchange](https://github.com/tinue/SharpDataExchange), see
the User Guide's [COM ports](docs/User-Guide.md#7-com-ports) chapter;
implementation details are in
[docs/PC1600-Serial-Port.md](docs/PC1600-Serial-Port.md).

## License

Calc-U-1600 is free software, licensed under the [GNU General Public
License, version 3](LICENSE) (GPLv3).

It's built on [Qt6](https://www.qt.io/), used under the LGPLv3 -- see
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for Qt and other
third-party components (including the `roms/` ROM images, which are
third-party copyrighted material not covered by this project's license).
