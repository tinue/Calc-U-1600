#!/bin/sh
# Rebuild the sibling SharpDataExchangeRust checkout and refresh the vendored
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
SRC=${SHARPDX_SRC:-../SharpDataExchangeRust}
DST="$REPO_ROOT/Core/Basic/vendor/sharpdx"

if [ ! -d "$SRC" ]; then
  echo "SharpDataExchangeRust checkout not found at $SRC" >&2
  echo "(set SHARPDX_SRC=/path/to/SharpDataExchangeRust to override)" >&2
  exit 1
fi

cd "$SRC"

# Match Calc-U-1600's minimum deployment target so `ld` doesn't warn about
# object files built for a newer macOS than the app links against.
export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-15.6}

if [ "$1" = "--universal" ]; then
  rustup target add x86_64-apple-darwin aarch64-apple-darwin >/dev/null 2>&1 || true
  cargo build --release --target aarch64-apple-darwin
  cargo build --release --target x86_64-apple-darwin
  lipo -create \
    target/aarch64-apple-darwin/release/libsharpdx.a \
    target/x86_64-apple-darwin/release/libsharpdx.a \
    -output "$DST/libsharpdx.a"
else
  cargo build --release
  cp target/release/libsharpdx.a "$DST/libsharpdx.a"
fi

cp include/sharpdx.h "$DST/sharpdx.h"

echo "refreshed $DST:"
cd "$REPO_ROOT"
lipo -info "$DST/libsharpdx.a"
ls -l "$DST/libsharpdx.a" "$DST/sharpdx.h"
