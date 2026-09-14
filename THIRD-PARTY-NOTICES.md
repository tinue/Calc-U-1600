# Third-Party Notices

## Qt6

Calc-U-1600's Qt6/ desktop app is built on [Qt6](https://www.qt.io/), (c)
The Qt Company Ltd and other contributors. It uses the Qt Widgets module
and its transitive dependencies (QtCore, QtGui, QtNetwork, QtDBus, and
related platform-integration modules).

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
sibling SharpDataExchangeRust project (own license terms; not part of
this project's GPLv3 source).

ROM images under `roms/` are third-party copyrighted firmware belonging
to Sharp Corporation. They are **not** covered by this project's GPLv3
license -- see [LICENSE](LICENSE) for Calc-U-1600's own code, and treat
`roms/` as a separate, unlicensed-by-us inclusion.
