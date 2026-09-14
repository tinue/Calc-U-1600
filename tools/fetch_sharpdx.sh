#!/bin/sh
# Downloads a prebuilt libsharpdx (+ header) from a SharpDataExchangeRust
# GitHub release and vendors it under Core/Basic/vendor/sharpdx/, so BASIC
# program loading (Core/Basic/BasicProgramSource.cpp, and everything that
# depends on it -- see Qt6/CMakeLists.txt) can build on platforms other
# than macOS without a local Rust toolchain.
#
# Counterpart to tools/refresh_sharpdx.sh, which instead builds from a
# local SharpDataExchangeRust source checkout -- use that one on macOS if
# you're developing sharpdx itself, or to refresh the committed macOS
# binary. This script is for Linux/Windows, where there's no vendored
# binary to refresh: it just downloads one. Unlike the macOS binary, the
# files this writes are NOT meant to be committed (see .gitignore) --
# they're a local/CI-only build artifact, fetched fresh each time.
#
# Usage: tools/fetch_sharpdx.sh
# Env vars:
#   SHARPDX_TAG       release tag to fetch (default: v0.1.2)
#   SHARPDX_PLATFORM  override auto-detected platform: linux-x86_64,
#                     linux-aarch64, or windows-x86_64
#
# Writes:
#   Core/Basic/vendor/sharpdx/sharpdx.h                 (header)
#   Core/Basic/vendor/sharpdx/libsharpdx-linux.a        (Linux)
#   Core/Basic/vendor/sharpdx/sharpdx.lib               (Windows)
#   Core/Basic/vendor/sharpdx/native-libs-windows.txt   (Windows, if the
#     release includes it -- see Qt6/CMakeLists.txt: the extra Windows
#     system import libs (ws2_32, etc) Rust's std needs, straight from
#     rustc's own --print=native-static-libs for that build, not a
#     hand-maintained guess. Older releases (< the one that added this
#     capture in package.sh) won't have the file; CMakeLists falls back to
#     a hardcoded list in that case.)
#
# The two library filenames are platform-specific on purpose, so a fetch
# here can never collide with -- or dirty the git status of -- the
# committed macOS libsharpdx.a. sharpdx.h, however, IS the same tracked
# path the macOS binary uses (BasicProgramSource.cpp hardcodes `#include
# "vendor/sharpdx/sharpdx.h"`, so there's nowhere else for it to live) --
# running this script WILL locally modify that tracked file if the fetched
# release's header has drifted from whatever's committed (same as running
# tools/refresh_sharpdx.sh does already). That's expected in CI (a fresh,
# never-committed checkout) but on a local Linux/Windows dev machine,
# don't commit the resulting sharpdx.h change unless you mean to bump the
# vendored version on purpose -- `git checkout -- Core/Basic/vendor/sharpdx/sharpdx.h`
# reverts it.
set -eu
cd "$(dirname "$0")/.."
REPO_ROOT=$(pwd)
DST="$REPO_ROOT/Core/Basic/vendor/sharpdx"
TAG=${SHARPDX_TAG:-v0.1.2}

detect_platform() {
  os=$(uname -s)
  arch=$(uname -m)
  case "$os" in
    Linux)
      case "$arch" in
        x86_64) echo linux-x86_64 ;;
        aarch64|arm64) echo linux-aarch64 ;;
        *) echo "fetch_sharpdx.sh: unsupported Linux arch '$arch'" >&2; exit 1 ;;
      esac
      ;;
    MINGW*|MSYS*|CYGWIN*)
      # Git Bash / MSYS on a Windows CI runner (`shell: bash` in Actions).
      echo windows-x86_64
      ;;
    Darwin)
      echo "fetch_sharpdx.sh: macOS already has a committed binary -- use tools/refresh_sharpdx.sh instead" >&2
      exit 1
      ;;
    *)
      echo "fetch_sharpdx.sh: unsupported OS '$os'" >&2; exit 1 ;;
  esac
}

PLATFORM=${SHARPDX_PLATFORM:-$(detect_platform)}
case "$PLATFORM" in
  linux-x86_64|linux-aarch64) EXT=tar.gz ;;
  windows-x86_64)             EXT=zip ;;
  *) echo "fetch_sharpdx.sh: unknown platform '$PLATFORM'" >&2; exit 1 ;;
esac

ARCHIVE="sharpdx-${PLATFORM}.${EXT}"
URL="https://github.com/tinue/SharpDataExchangeRust/releases/download/${TAG}/${ARCHIVE}"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "fetch_sharpdx.sh: downloading $URL"
curl -sSfL -o "$WORK/$ARCHIVE" "$URL"

cd "$WORK"
case "$EXT" in
  tar.gz) tar xzf "$ARCHIVE" ;;
  zip)
    if command -v unzip >/dev/null 2>&1; then unzip -q "$ARCHIVE"
    else tar xf "$ARCHIVE"  # Windows' built-in tar.exe (bsdtar) also reads zip
    fi
    ;;
esac

STAGE="$WORK/sharpdx-${PLATFORM}"
mkdir -p "$DST"
cp "$STAGE/include/sharpdx.h" "$DST/sharpdx.h"

case "$PLATFORM" in
  windows-x86_64)
    cp "$STAGE/lib/sharpdx.lib" "$DST/sharpdx.lib"
    if [ -f "$STAGE/lib/native-libs-windows.txt" ]; then
      cp "$STAGE/lib/native-libs-windows.txt" "$DST/native-libs-windows.txt"
    else
      echo "fetch_sharpdx.sh: release $TAG has no native-libs-windows.txt (older release?) -- CMakeLists will fall back to its hardcoded list" >&2
      rm -f "$DST/native-libs-windows.txt"
    fi
    ;;
  linux-*)
    cp "$STAGE/lib/libsharpdx.a" "$DST/libsharpdx-linux.a"
    ;;
esac

echo "fetch_sharpdx.sh: vendored $PLATFORM sharpdx ($TAG) into $DST"
ls -l "$DST"
