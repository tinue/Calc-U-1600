# Calc-U-1600

Calc-U-1600 is an emulator for the Sharp PC-1500, PC-1500A, and PC-1600
pocket computers, built as a Qt6 desktop app (macOS / Linux / Windows).

## Status

All three machines boot to BASIC with keyboard, dot-matrix display, RTC,
expansion-card, and serial-port support. See [TODO.md](TODO.md) for known
issues and open work.

## Documentation

- [docs/User-Guide.md](docs/User-Guide.md) — using the emulator: the
  keyboard, memory modules and plotter, saving battery-backed module
  state, Settings, and advanced topics (Debug panel, custom YAML memory
  cards, presets).
- [docs/Building.md](docs/Building.md) — building from source
  (prerequisites, ROMs, per-platform build steps).

## Serial port

The emulated PC-1600's RS-232C port is always live and backed by a host
pseudo-terminal. Point any Mac serial application (SharpDataExchange, a
terminal program) at the stable `calcu1600.serial` symlink in the folder
set under **Settings ▸ Serial Port**. On the PC-1600 side, select
XON/XOFF flow control (`SETCOM "COM1:",9600,8,N,1,X,N`) so it rides the
byte stream. See [docs/PC1600-Serial-Port.md](docs/PC1600-Serial-Port.md).

## License

Calc-U-1600 is free software, licensed under the [GNU General Public
License, version 3](LICENSE) (GPLv3).

It's built on [Qt6](https://www.qt.io/), used under the LGPLv3 -- see
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for Qt and other
third-party components (including the `roms/` ROM images, which are
third-party copyrighted material not covered by this project's license).
