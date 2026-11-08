#!/bin/sh
# Rebuild the sibling SharpDataExchange checkout and refresh the vendored
# static library + header under Core/Basic/vendor/sharpdx/.
#
# Usage:
#   tools/refresh_sharpdx.sh              # arm64 (dev default)
#   tools/refresh_sharpdx.sh --universal  # arm64 + x86_64, lipo'd fat archive
#
# Commit the changed Core/Basic/vendor/sharpdx/{libsharpdx.a,sharpdx.h} after.
set -e
cd "$(dirname "$0")/.."
REPO_ROOT=$(pwd)
SRC=${SHARPDX_SRC:-../SharpDataExchange}
DST="$REPO_ROOT/Core/Basic/vendor/sharpdx"

if [ ! -d "$SRC" ]; then
  echo "SharpDataExchange checkout not found at $SRC" >&2
  echo "(set SHARPDX_SRC=/path/to/SharpDataExchange to override)" >&2
  exit 1
fi

cd "$SRC"

# Match Calc-U-1600's minimum deployment target (Qt6/CMakeLists.txt's
# CMAKE_OSX_DEPLOYMENT_TARGET) so `ld` doesn't warn about object files built
# for a newer macOS than the app links against. SharpDataExchange pins the
# same value in its .cargo/config.toml; this export just makes it explicit.
export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-15.8}

# Library only, without the `serial` transport (the default features pull it
# in for the `sde` binary) -- same serial-free build the release archives
# ship, so there's no serialport/IOKit code to link.
CARGO_FLAGS="--release --lib --no-default-features"

# Refuse a library whose objects claim a newer macOS than the deployment
# target. Not every Rust toolchain ships std built for rustc's default
# (11.0): one whose std was compiled on the build machine stamps every std
# object with that machine's macOS (26.0, 27.0, ...), and the app's link
# then warns "built for newer 'macOS' version". Use rustup's toolchain.
check_minos() {
  newest=$(otool -l "$1" | awk '$1 == "minos" {print $2}' | sort -uV | tail -1)
  if [ -z "$newest" ]; then
    echo "$1: no minos found (otool -l)" >&2
    exit 1
  fi
  top=$(printf '%s\n%s\n' "$MACOSX_DEPLOYMENT_TARGET" "$newest" | sort -V | tail -1)
  if [ "$top" != "$MACOSX_DEPLOYMENT_TARGET" ]; then
    echo "$1: objects built for macOS $newest, newer than $MACOSX_DEPLOYMENT_TARGET -- not vendoring it" >&2
    echo "(rustc: $(command -v rustc), $(rustc -V))" >&2
    otool -l "$1" | awk '$1 == "minos" {print $2}' | sort -V | uniq -c >&2
    exit 1
  fi
}

if [ "$1" = "--universal" ]; then
  rustup target add x86_64-apple-darwin aarch64-apple-darwin >/dev/null 2>&1 || true
  cargo build $CARGO_FLAGS --target aarch64-apple-darwin
  cargo build $CARGO_FLAGS --target x86_64-apple-darwin
  check_minos target/aarch64-apple-darwin/release/libsharpdx.a
  check_minos target/x86_64-apple-darwin/release/libsharpdx.a
  lipo -create \
    target/aarch64-apple-darwin/release/libsharpdx.a \
    target/x86_64-apple-darwin/release/libsharpdx.a \
    -output "$DST/libsharpdx.a"
else
  cargo build $CARGO_FLAGS
  check_minos target/release/libsharpdx.a
  cp target/release/libsharpdx.a "$DST/libsharpdx.a"
fi

cp include/sharpdx.h "$DST/sharpdx.h"

echo "refreshed $DST:"
cd "$REPO_ROOT"
lipo -info "$DST/libsharpdx.a"
ls -l "$DST/libsharpdx.a" "$DST/sharpdx.h"
