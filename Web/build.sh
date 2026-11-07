#!/usr/bin/env bash
# Builds the web client into Web/dist/ (gitignored): the WASM engine (Core/ +
# Shared/ + Web/bindings.cpp, CMake target calcu_web), the files it loads
# (ROMs, bundled cards and disks, the curated presets), the faceplates, the
# client script and stylesheet, the installable app (app/) and a demo page.
# docs/developer/Web-Client.md is the contract for what dist/ contains.
#
# Needs emcmake/cmake (Homebrew `emscripten` or an activated emsdk; CI pins
# the version), ninja, node, and ROMs in roms/ (tools/fetch_roms.sh).
set -euo pipefail

WEB="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$WEB/.." && pwd)"
DIST="$WEB/dist"
BUILD="$REPO/build-wasm"

for tool in emcmake cmake node; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "error: $tool not found on PATH." >&2
    exit 1
  fi
done
if ! ls "$REPO"/roms/*.bin >/dev/null 2>&1; then
  echo "error: no ROMs in roms/ -- run tools/fetch_roms.sh." >&2
  exit 1
fi

if [ ! -f "$BUILD/CMakeCache.txt" ]; then
  emcmake cmake -S "$REPO" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
fi
cmake --build "$BUILD" --target calcu_web

rm -rf "$DIST"
mkdir -p "$DIST/wasm" "$DIST/bundle" "$DIST/img" "$DIST/app" "$DIST/vendor"

cp "$BUILD/calcu1600-core.js" "$BUILD/calcu1600-core.wasm" "$DIST/wasm/"
# What the engine reads as bundled resources (AppPaths::bundledResourcesDir):
# every ROM image and the bundled card and disk templates. The host-drive
# ROM stays out: the web has no host drive.
find "$REPO/roms" -maxdepth 1 -type f \( -iname '*.bin' -o -iname '*.rom' \) -exec cp {} "$DIST/bundle/" \;
cp "$REPO"/Qt6/resources/cards/*.yaml "$DIST/bundle/"

cp "$REPO"/Qt6/resources/faceplates/*.png "$DIST/img/"
cp "$REPO/Qt6/resources/linux/calc-u-1600.png" "$DIST/img/icon.png"
cp "$REPO/Qt6/resources/linux/calc-u-1600.png" "$DIST/app/icon.png"

cp "$WEB/index.html" "$WEB/calc.css" "$WEB/calc-engine-worker.js" "$WEB/audio-worklet.js" "$DIST/"
# React 18 for the app and the demo page; a host page brings its own.
cp "$WEB/vendor/react.production.min.js" "$WEB/vendor/react-dom.production.min.js" "$DIST/vendor/"
cp "$WEB/app/index.html" "$WEB/app/app.css" "$WEB/app/sw.js" "$WEB/app/engine-worker.js" "$WEB/app/manifest.json" "$DIST/app/"

node "$WEB/build.js"

echo "wrote $DIST"
