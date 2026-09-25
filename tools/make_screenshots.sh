#!/usr/bin/env bash
# Regenerates the user-guide screenshots (docs/images/) by playing the
# scenarios in docs/screenshots/ against the Qt app -- see
# docs/screenshots/README.md. Builds the app first if needed.
#
#   tools/make_screenshots.sh              # every docs/screenshots/*.shots.yaml
#   tools/make_screenshots.sh user-guide   # just these (names without .shots.yaml)
#
# Extra --shots-* options can follow the names after `--`, e.g.
#   tools/make_screenshots.sh user-guide -- --shots-only settings
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/Qt6/build"

if [[ ! -f "${BUILD_DIR}/build.ninja" ]]; then
  cmake -S "${ROOT}/Qt6" -B "${BUILD_DIR}" -G Ninja -DCMAKE_BUILD_TYPE=Release
fi
cmake --build "${BUILD_DIR}"

if [[ "$(uname -s)" == "Darwin" ]]; then
  APP="${BUILD_DIR}/Calc-U-1600.app/Contents/MacOS/Calc-U-1600"
else
  APP="${BUILD_DIR}/Calc-U-1600"
fi

names=()
extra=()
while [[ $# -gt 0 ]]; do
  if [[ "$1" == "--" ]]; then shift; extra=("$@"); break; fi
  names+=("$1"); shift
done

scenarios=()
if [[ ${#names[@]} -eq 0 ]]; then
  scenarios=("${ROOT}"/docs/screenshots/*.shots.yaml)
else
  for n in "${names[@]}"; do scenarios+=("${ROOT}/docs/screenshots/${n}.shots.yaml"); done
fi

status=0
for s in "${scenarios[@]}"; do
  "${APP}" --shots "${s}" ${extra[@]+"${extra[@]}"} || status=1
done
exit ${status}
