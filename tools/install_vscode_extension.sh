#!/bin/sh
# Packages vscode/calcu1600-debug as a .vsix (into headless/) and installs
# it into VS Code. Re-run after changing the extension, then reload the
# VS Code window. Current VS Code ignores extension folders copied or
# symlinked into ~/.vscode/extensions by hand ("marked as removed"), so
# installing through `code --install-extension` is the way.
set -e
cd "$(dirname "$0")/.."
mkdir -p headless
(cd vscode/calcu1600-debug &&
  npx --yes @vscode/vsce package --allow-missing-repository --skip-license -o ../../headless/calcu1600-debug.vsix)
code --install-extension headless/calcu1600-debug.vsix --force
