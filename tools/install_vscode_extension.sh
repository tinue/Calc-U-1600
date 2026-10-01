#!/bin/sh
# Packages vscode/calcu1600-debug as a .vsix (into headless/) and installs
# it into VS Code. Re-run after changing the extension, then reload the
# VS Code window. Current VS Code ignores extension folders copied or
# symlinked into ~/.vscode/extensions by hand ("marked as removed"), so
# installing through `code --install-extension` is the way. Packaging
# needs only python3 (see package_vscode_extension.py for why not vsce).
set -e
cd "$(dirname "$0")/.."
mkdir -p headless
python3 tools/package_vscode_extension.py vscode/calcu1600-debug headless/calcu1600-debug.vsix
code --install-extension headless/calcu1600-debug.vsix --force
