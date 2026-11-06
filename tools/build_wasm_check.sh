#!/bin/sh
# Builds Core + Shared for WebAssembly with Emscripten and runs the node
# smoke test (tools/wasm_smoke.cpp) from the repo root: the check that the
# frontend-neutral layers stay portable to a browser GUI (C++20, no Qt,
# no threads, no blocking -- docs/developer/Shared-Architecture.md).
#
# Needs emcc/emcmake (Homebrew `emscripten`, or an activated emsdk), node,
# and Core/Basic/vendor/sharpdx/libsharpdx-wasm.a (committed; rebuild with
# tools/build_sharpdx_wasm.sh). ROMs in roms/ (tools/fetch_roms.sh) for the
# smoke run; without them it builds and skips.
set -e
cd "$(dirname "$0")/.."
if [ ! -f build-wasm/CMakeCache.txt ]; then
  emcmake cmake -S . -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
fi
cmake --build build-wasm --target wasm_smoke
set +e
node build-wasm/wasm_smoke.js
status=$?
set -e
if [ $status -eq 2 ]; then exit 0; fi  # no ROMs: built, smoke skipped
exit $status
