# Third-Party Notices

## Qt6

Calc-U-1600's Qt6/ desktop app is built on [Qt6](https://www.qt.io/), (c)
The Qt Company Ltd and other contributors. It uses the Qt Widgets, Qt Multimedia
and Qt Network modules and their transitive dependencies (QtCore, QtGui,
QtDBus, and related platform-integration modules).

Qt6 is used here under the **GNU Lesser General Public License, version 3**
(LGPLv3) -- see [licenses/LGPL-3.0.txt](licenses/LGPL-3.0.txt), which
incorporates the [GNU General Public License, version 3](licenses/GPL-3.0.txt)
by reference. Qt is linked dynamically (as shared frameworks/libraries,
not statically), and unmodified -- no changes are made to Qt's own source.

Complete corresponding source for the exact Qt6 version used in each
release is available free of charge from The Qt Company:

- <https://www.qt.io/download-open-source>
- <https://download.qt.io/official_releases/qt/>

The macOS/Linux/Windows builds published by this project's CI record the
exact Qt version installed for that build in the GitHub Actions run log
(`.github/workflows/build.yml`).

## Other bundled components

See `Core/Basic/vendor/sharpdx/` for `libsharpdx`, vendored from the
sibling SharpDataExchange project (own license terms; not part of
this project's GPLv3 source).

ROM images under `roms/` are third-party copyrighted firmware belonging
to Sharp Corporation. They are **not** covered by this project's GPLv3
license -- see [LICENSE](LICENSE) for Calc-U-1600's own code, and treat
`roms/` as a separate, unlicensed-by-us inclusion.

The same applies to ROM content bundled inside memory-module definitions
under `Qt6/resources/cards/` — currently `ce502b.card.yaml`, Sharp's
CE-502B Statistics module, from Jeff Birt's dump
(https://github.com/Jeff-Birt/PC-1500_ROM_Modules).

`examples/dwx/S3/dwx.bin` is the PC-1600 DiskWorks v3 program by
Christian Becker (KiKiSoft, 1993-2026) -- see
[github.com/hzprky/DiskWorks](https://github.com/hzprky/DiskWorks) and
`examples/dwx/DiskWorks.pc1600`. `examples/machine-code/CALCULAT.BIN` is
CalCula, a PC-1600 RPN calculator by the same author (KiKiSoft, 1993) --
see `examples/machine-code/Calculat.pc1600`. Both are bundled with the
author's permission; not covered by this project's GPLv3 license.

`examples/setup/util_15.bas`, `utilrm_15.bas`, `utilrm_20.bas`, and the
`firmware_bootstrap_util*.pc1500a` presets that load them are CE-163F
firmware-flashing utilities from [Soigeneris](https://www.soigeneris.com/sharp-pc-1500-memory-modules).
Bundled with permission; not covered by this project's GPLv3 license.

## Acknowledgments

**Jeff Birt** dumped the PC-1500 system ROMs (A01, A03, A04), the CE-158
ROM and the CE-502B module, and wrote the annotated
[PC-1500 ROM disassembly](https://github.com/Jeff-Birt/Sharp_PC-1500_ROM_Disassembly)
(for TASM). Its routine names are used throughout the PC-1500 docs, among
them [PC1500-Keyword-Modules.md](docs/PC1500-Keyword-Modules.md).
`roms/README.md` lists which dumps come from him.

**Paul Chambre**'s analysis of how a PC-1500 ROM module adds BASIC keywords
was the starting point for [PC1500-Keyword-Modules.md](docs/PC1500-Keyword-Modules.md)
and the RENUM template. The calling convention for keyword *functions*
there comes from that analysis and isn't checked against the ROM.

**MEP rev3, (c) spellbound, 2024.** The host-directory drive (**File ▸
Mount Directory…**) is based on the MEP rev3 (Modular Extension Platform)
with its USB memory-stick application, a 60-pin bus module for the
PC-1600. The MEP provided:

- the idea of offering external storage as the PC-1600 file device `S3:`
  (alias `Y:`), through a ROM module in page-1 bank 7 with a controller
  behind I/O port 90H;
- the BASIC statements `CDIR` and `LDIR`, with the same keyword tokens, so
  tokenized programs run on both;
- the machine-code entry points CDIR (`&4020`), DIRMODE (`&4023`) and
  FILEMODE (`&4026`) in bank 7, and the CDIR prompt at `&FB10`, so software
  written for the MEP, such as FILEX, runs unchanged.

Calc-U-1600's driver ROM, its protocol to the host and the host side are
its own work, and no MEP code is included. The MEP rev3 manual served as
the reference for the public interface.
