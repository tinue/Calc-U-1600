#!/bin/sh
# Downloads the Sharp firmware ROM dumps this project needs into roms/,
# from their public upstream sources (see roms/README.md for provenance
# detail on each file). ROMs are not in this repository (Sharp
# Corporation's copyrighted firmware) -- so this script is how a fresh
# clone (or CI) gets a working roms/ directory before building.
#
# Idempotent: skips a file that's already present and matches its known
# md5, so re-running after a partial/interrupted fetch is safe and cheap.
#
# Usage: tools/fetch_roms.sh
#
# Writes (13 files):
#   roms/PC-1500_A01.ROM, PC-1500_A03.ROM, PC-1500_A04.ROM
#     -- from Jeff-Birt/Sharp_PC-1500_ROM_Disassembly (Original_ROMs/)
#   roms/PC1600-*.bin (8 files)
#     -- from tinue/PC-1600-ROM (dumps/, uppercase .BIN upstream, renamed
#        lowercase .bin here to match this repo's existing convention)
#   roms/CE-158.ROM
#     -- from Jeff-Birt/Sharp_CE-158 (CE-158_ROM_ORIG.bin)
#   roms/CE-150.ROM
#     -- from tinue/PC-1500-ROM (dumps/CE-150.BIN)
set -eu

cd "$(dirname "$0")/.."
ROMS_DIR=roms

log() { echo "fetch_roms.sh: $*" >&2; }
die() { log "error: $*"; exit 1; }

# md5 (BSD/macOS) vs md5sum (Linux) -- print just the hex digest.
md5_of() {
  if command -v md5 >/dev/null 2>&1; then
    md5 -q "$1"
  else
    md5sum "$1" | awk '{print $1}'
  fi
}

# fetch_if_needed URL DEST_PATH EXPECTED_MD5
fetch_if_needed() {
  url=$1
  dest=$2
  expected_md5=$3

  if [ -f "$dest" ] && [ "$(md5_of "$dest")" = "$expected_md5" ]; then
    log "up to date: $dest"
    return 0
  fi

  log "fetching $dest <- $url"
  tmp="$dest.tmp$$"
  if ! curl -fsSL "$url" -o "$tmp"; then
    rm -f "$tmp"
    die "download failed: $url"
  fi

  actual_md5=$(md5_of "$tmp")
  if [ "$actual_md5" != "$expected_md5" ]; then
    rm -f "$tmp"
    die "checksum mismatch for $dest (got $actual_md5, expected $expected_md5) -- upstream file may have changed, update this script"
  fi
  mv "$tmp" "$dest"
}

mkdir -p "$ROMS_DIR"

# ---- PC-1500 (Jeff-Birt/Sharp_PC-1500_ROM_Disassembly, Original_ROMs/) --
PC1500_BASE="https://raw.githubusercontent.com/Jeff-Birt/Sharp_PC-1500_ROM_Disassembly/main/Original_ROMs"
fetch_if_needed "$PC1500_BASE/PC-1500_A01.ROM" "$ROMS_DIR/PC-1500_A01.ROM" fbc55a9a8743e619b7709721ff5bcbff
fetch_if_needed "$PC1500_BASE/PC-1500_A03.ROM" "$ROMS_DIR/PC-1500_A03.ROM" 4bcf78a6d3d32e2a0349eb2d28987b8d
fetch_if_needed "$PC1500_BASE/PC-1500_A04.ROM" "$ROMS_DIR/PC-1500_A04.ROM" 8ebec8b0ef358645df14807c31df7d06

# ---- PC-1600 (tinue/PC-1600-ROM, dumps/) -- upstream files are
# uppercase .BIN; renamed lowercase .bin here to match this repo's
# existing filenames (Qt6/CMakeLists.txt globs *.bin, case-sensitive). ----
PC1600_BASE="https://raw.githubusercontent.com/tinue/PC-1600-ROM/main/dumps"
fetch_if_needed "$PC1600_BASE/PC1600-LH5803-C000-FFFF.BIN" "$ROMS_DIR/PC1600-LH5803-C000-FFFF.bin" 56168830b46d637b08529a74609bee3f
fetch_if_needed "$PC1600_BASE/PC1600-P0-B0.BIN"            "$ROMS_DIR/PC1600-P0-B0.bin"            404bf6f2df489e09649167078acd9a25
fetch_if_needed "$PC1600_BASE/PC1600-P1-B0.BIN"             "$ROMS_DIR/PC1600-P1-B0.bin"            bddbb8bbf0b2bd2d95038f67b8d002ac
fetch_if_needed "$PC1600_BASE/PC1600-P1-B3.BIN"             "$ROMS_DIR/PC1600-P1-B3.bin"            6f6e1a9d46db7dc91d4322c93583ac81
fetch_if_needed "$PC1600_BASE/PC1600-P1-B3B.BIN"            "$ROMS_DIR/PC1600-P1-B3B.bin"           2483319acf35da4e848e59ab954abf46
fetch_if_needed "$PC1600_BASE/PC1600-P1-B4-CE1600P.BIN"     "$ROMS_DIR/PC1600-P1-B4-CE1600P.bin"    05548a8dda3e572d50d4bd281a650ea8
fetch_if_needed "$PC1600_BASE/PC1600-P1-B5-CE1600P-OR-F.BIN" "$ROMS_DIR/PC1600-P1-B5-CE1600P-OR-F.bin" a675c6dbdf7dc4c10e8d96891e196f8f
fetch_if_needed "$PC1600_BASE/PC1600-P2-B6.BIN"             "$ROMS_DIR/PC1600-P2-B6.bin"            86cb9036da284de2b04c7946d140a9fd

# ---- CE-158 (Jeff-Birt/Sharp_CE-158) ----
fetch_if_needed "https://raw.githubusercontent.com/Jeff-Birt/Sharp_CE-158/main/CE-158_ROM_ORIG.bin" "$ROMS_DIR/CE-158.ROM" aa952878fb29da4844791d95185649ca

# ---- CE-150 (tinue/PC-1500-ROM, dumps/) ----
fetch_if_needed "https://raw.githubusercontent.com/tinue/PC-1500-ROM/main/dumps/CE-150.BIN" "$ROMS_DIR/CE-150.ROM" eb9aa5156c6849890b137799efc50a4b

log "done"
