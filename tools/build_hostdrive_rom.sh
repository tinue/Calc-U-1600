#!/bin/sh
# Assemble the PC-1600 host-directory drive ROM (firmware/pc1600-hostdrive/)
# into PC1600-P1-B7-HOSTDRIVE.bin + .lst next to the source. Both outputs are
# committed; rerun this after editing hostdrive.asm.
#
# zasm: $CALCU_ZASM, default ~/Development/sharp/zasm/zasm (as in
# the VS Code extension, calcu1600.zasmPath).
set -eu

ZASM="${CALCU_ZASM:-$HOME/Development/sharp/zasm/zasm}"
DIR="$(cd "$(dirname "$0")/../firmware/pc1600-hostdrive" && pwd)"

"$ZASM" -uwy "$DIR/hostdrive.asm" "$DIR/PC1600-P1-B7-HOSTDRIVE.lst" "$DIR/PC1600-P1-B7-HOSTDRIVE.bin"

size=$(wc -c < "$DIR/PC1600-P1-B7-HOSTDRIVE.bin" | tr -d ' ')
if [ "$size" -ne 16384 ]; then
    echo "error: PC1600-P1-B7-HOSTDRIVE.bin is $size bytes, expected 16384" >&2
    exit 1
fi
echo "PC1600-P1-B7-HOSTDRIVE.bin: $size bytes"
