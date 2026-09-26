#!/usr/bin/env bash
# Configures (if needed), builds, and launches the Qt6 Calc-U-1600 app.
# macOS-first (spawns the .app bundle via `open`); on Linux it runs the
# built binary directly instead.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/Qt6/build"

"${SCRIPT_DIR}/tools/build_app.sh"

if [[ "$(uname -s)" == "Darwin" ]]; then
  open "${BUILD_DIR}/Calc-U-1600.app"
  # `open` returns immediately after handing off to launchd; without a
  # short pause here, a terminal window closed right after this script
  # exits can trigger iTerm's "session ended before its child process
  # finished" warning even though the app launched fine.
  sleep 2
else
  exec "${BUILD_DIR}/Calc-U-1600"
fi
