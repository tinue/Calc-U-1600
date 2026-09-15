# Building Calc-U-1600

This is the developer build guide: prerequisites, getting the ROMs, and
building the Qt6 desktop app on macOS, Linux, and Windows. For how to
*use* the emulator once it's built, see [User-Guide.md](User-Guide.md).

Everything here is derived from the project's own recipes —
[`Qt6/CMakeLists.txt`](../Qt6/CMakeLists.txt) and the CI workflow at
[`.github/workflows/build.yml`](../.github/workflows/build.yml) — which is
also where to look first if a step below stops matching reality. The
macOS steps are exercised on an actual Mac as part of writing this doc;
the Linux and Windows steps are transcribed from CI and not independently
verified on a fresh machine — if something's off, please open an issue or
PR.

## Prerequisites

- **CMake** 3.21 or newer.
- **Qt 6.3 or newer** (the `Widgets` component only). Older Qt6 (e.g.
  Ubuntu 22.04's packaged 6.2.4) doesn't provide
  `qt_standard_project_setup()` and won't configure.
- A **C++17** compiler (the project is plain portable C++, no
  compiler-specific extensions beyond what Qt itself needs).
- On macOS only: Xcode command-line tools (for the Objective-C++
  compiler — one file, `MacClipboardImage.mm`, needs it for the
  plotter-paper clipboard export).

### Optional: BASIC preset loading (`libsharpdx`)

Loading BASIC programs from `.pc1500`/`.pc1600` preset files (`type:`
typing and the fast `basic-binary` loader) depends on a vendored Rust
static library, `Core/Basic/vendor/sharpdx/`:

- On **macOS** it's committed directly to the repo — nothing to do.
- On **Linux/Windows** it's fetched at configure time by
  `tools/fetch_sharpdx.sh` (see below) from a `SharpDataExchangeRust`
  GitHub release.

This is a *soft* dependency: if the library isn't present, CMake skips
those source files and the app still builds and runs — "Open Preset…"
just reports the feature unavailable. You don't need a Rust toolchain
either way; only a prebuilt `.a`/`.lib` is needed, and CI fetches or
commits it for you.

## 1. Get the source

```sh
git clone <this repository's URL>
cd Calc-U-1600
```

## 2. Get the ROMs

The emulator needs real Sharp ROM images, which aren't committed to the
repo (see [`roms/README.md`](../roms/README.md) for provenance/licensing).
Fetch and verify them before configuring:

```sh
tools/fetch_roms.sh
```

Skipping this step doesn't fail the build — CMake just globs an empty
`roms/` directory — but the resulting binary ships with no firmware and
won't boot any machine.

If you also want BASIC preset loading on Linux/Windows (see above):

```sh
tools/fetch_sharpdx.sh
```

## 3. Configure & build

### macOS

```sh
brew install qt cmake ninja
cmake -S Qt6 -B Qt6/build -G Ninja \
  -DCMAKE_PREFIX_PATH=$(brew --prefix qt) \
  -DCMAKE_BUILD_TYPE=Release
cmake --build Qt6/build
```

This produces `Qt6/build/Calc-U-1600.app`, a real macOS bundle. To run it
standalone outside this checkout (e.g. hand it to another Mac), also run
`macdeployqt` to embed the Qt frameworks:

```sh
$(brew --prefix qt)/bin/macdeployqt Qt6/build/Calc-U-1600.app
```

Without that step the app still runs from this build directory (Qt is
found via the Homebrew prefix), just not as a portable, relocatable
bundle. The signing/notarizing/DMG steps CI does after this are
release-packaging concerns — see the `mac-aarch64` job in
[`.github/workflows/build.yml`](../.github/workflows/build.yml).

### Linux (best-effort — CI-derived, not verified on a fresh machine)

```sh
sudo apt-get install -y \
  build-essential cmake ninja-build patchelf \
  qt6-base-dev qt6-base-dev-tools qt6-wayland libgl1-mesa-dev
cmake -S Qt6 -B Qt6/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build Qt6/build
```

This produces a plain `Qt6/build/Calc-U-1600` executable plus a
`Qt6/build/resources/` folder (bundled `.card.yaml` definitions and ROMs)
that must stay next to it. That's enough to run on the machine that built
it. To package a standalone `.AppImage` that runs on other Linux machines,
see the `linux-x86_64`/`linux-aarch64` jobs in
[`.github/workflows/build.yml`](../.github/workflows/build.yml) for the
full `linuxdeploy`/`linuxdeploy-plugin-qt` recipe (it's involved enough —
Wayland plugin handling included — that it's not worth hand-transcribing
here; run the workflow or copy its steps directly). Linux arm64 CI is
`continue-on-error` in that workflow, i.e. best-effort even there.

### Windows (best-effort — CI-derived, not verified on a fresh machine)

```powershell
# Install Qt 6.8.x (MSVC, win64_msvc2022_64) and Ninja by whatever means
# you prefer (CI uses the aqtinstall-based jurplel/install-qt-action).
cmake -S Qt6 -B Qt6/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build Qt6/build
```

Run this from a shell with the MSVC toolchain on `PATH` (a Visual Studio
"Developer Command Prompt", or `vcvarsall.bat`/`ilammy/msvc-dev-cmd`'s
equivalent). This produces `Qt6/build/Calc-U-1600.exe` plus a
`Qt6/build/resources/` folder that must stay next to it. To stage a
standalone-runnable copy (Qt DLLs + MSVC runtime bundled in) and package
it as an installer:

```powershell
windeployqt --release --compiler-runtime Qt6\build\Calc-U-1600.exe
```

then package the staged folder with Inno Setup using
[`Qt6/resources/windows/calc-u-1600.iss`](../Qt6/resources/windows/calc-u-1600.iss)
— see the `windows-x86_64` job in `build.yml` for the exact staging
layout. The installer is currently unsigned.

CI also cross-compiles a native ARM64 build (`windows-arm64` job) from
the same x86_64 runner, using Qt's `win64_msvc2022_arm64` target and
`ilammy/msvc-dev-cmd`'s `amd64_arm64` cross toolchain, and passes
`/DAPP_ARCH=arm64` to `ISCC.exe` so the `.iss` above produces an
ARM64-only `Calc-U-1600-Setup-arm64.exe` instead. It skips BASIC preset
loading (no `windows-arm64` SharpDataExchangeRust release exists yet —
see `tools/fetch_sharpdx.sh`), otherwise identical to the x86_64 build.

## Running the tests

```sh
tools/run_tests.sh
```

Builds and runs the headless `Core/` test suite (no Qt, no GUI) from a
plain `clang++` invocation — must be run from the repo root, since the
tests load `roms/PC-1500_A04.ROM` via a relative path.

## Packaging

Release packaging (code signing, notarization, installers) is handled
by CI — see [`.github/workflows/build.yml`](../.github/workflows/build.yml)
and [`bin/release`](../bin/release) rather than duplicating it here. The
Windows installer is currently unsigned; every other platform's package
is signed (macOS) or otherwise complete.
