#!/usr/bin/env bash
# Configures (first time only) and builds the Qt6 app into Qt6/build.
# Shared by build_and_run.sh and tools/make_screenshots.sh.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/Qt6/build"

if [[ ! -f "${BUILD_DIR}/build.ninja" ]]; then
  cmake -S "${ROOT}/Qt6" -B "${BUILD_DIR}" -G Ninja -DCMAKE_BUILD_TYPE=Release
fi
cmake --build "${BUILD_DIR}"
