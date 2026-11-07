#!/bin/sh
# Builds and runs the headless Core/ + Shared/ test suite (CoreTests) at -O1,
# from the repo-root CMakeLists.txt without the Qt app, in build-tests/.
# The source lists come from cmake/CalcU.cmake and the root file's test
# glob, so there is nothing to keep in sync here. The tests run from the
# repo root (they load roms/ via relative paths).
set -e
cd "$(dirname "$0")/.."
if [ ! -f build-tests/CMakeCache.txt ]; then
  cmake -S . -B build-tests -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCALCU_BUILD_QT=OFF >/dev/null
fi
cmake --build build-tests --target CoreTests
build-tests/CoreTests "$@"
