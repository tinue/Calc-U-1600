#!/bin/sh
# Parses every preset in the repository (nothing is booted) and reports the
# ones that don't parse. Run it after a change to the preset format.
# Usage: tools/check_presets.sh   (needs the CMake build in build/)
set -e
cd "$(dirname "$0")/.."
cmake --build build --target pc1600_cli > /dev/null
# The VS Code templates hold {{name}} placeholders; they are checked by the
# extension's own tests once filled in.
git ls-files '*.pc1500' '*.pc1500a' '*.pc1600' | grep -v '^vscode/calcu1600-debug/templates/' |
  tr '\n' '\0' | xargs -0 ./build/pc1600_cli --check-preset
