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
# Writes (19 files):
#   roms/PC-1500_A01.ROM, PC-1500_A03.ROM, PC-1500_A04.ROM
#     -- from Jeff-Birt/Sharp_PC-1500_ROM_Disassembly (Original_ROMs/)
#   roms/PC1600-*-new.bin, PC1600-*-old.bin (6 + 6 files), plus the two
#   CE-1600P pages roms/PC1600-P1-B4-CE1600P.bin, -P1-B5-CE1600P-OR-F.bin
#     -- from tinue/PC-1600-ROM (dumps/new, dumps/old, dumps/peripherals;
#        uppercase .BIN upstream, renamed lowercase .bin with a -new/-old
#        suffix here)
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

# ---- PC-1600 (tinue/PC-1600-ROM) -- upstream files are uppercase .BIN in
# dumps/new/ (current ROM), dumps/old/ (older ROM) and dumps/peripherals/
# (CE-1600P). Renamed lowercase .bin here (Qt6/CMakeLists.txt globs *.bin,
# case-sensitive) and the calculator ROMs get a -new/-old suffix so both
# versions live side by side in the flat roms/ directory. ----
PC1600_BASE="https://raw.githubusercontent.com/tinue/PC-1600-ROM/main/dumps"

# fetch_pc1600_calc VERSION BASENAME MD5
fetch_pc1600_calc() {
  fetch_if_needed "$PC1600_BASE/$1/$2.BIN" "$ROMS_DIR/$2-$1.bin" "$3"
}

# New ROM (PEEK #(0,&7FFF) = 4 or 5)
fetch_pc1600_calc new PC1600-LH5803-C000-FFFF 56168830b46d637b08529a74609bee3f
fetch_pc1600_calc new PC1600-P0-B0             404bf6f2df489e09649167078acd9a25
fetch_pc1600_calc new PC1600-P1-B0             bddbb8bbf0b2bd2d95038f67b8d002ac
fetch_pc1600_calc new PC1600-P1-B3             6f6e1a9d46db7dc91d4322c93583ac81
fetch_pc1600_calc new PC1600-P1-B3B            2483319acf35da4e848e59ab954abf46
fetch_pc1600_calc new PC1600-P2-B6             86cb9036da284de2b04c7946d140a9fd

# Old ROM (PEEK #(0,&7FFF) = 130) -- unverified upstream.
# P1-B3B is TRUNCATED (16368 bytes): the app rejects it until it is redumped;
# replace its md5 then.
fetch_pc1600_calc old PC1600-LH5803-C000-FFFF 6005b6420bd5e191e81a1562f3242ec9
fetch_pc1600_calc old PC1600-P0-B0             5afcc22134e106bfd63b899febe9df7c
fetch_pc1600_calc old PC1600-P1-B0             3bcb6b178f5967c7c8e32afe560a3e75
fetch_pc1600_calc old PC1600-P1-B3             ded92d8280f8f83ce3498fb9cbfb9b3d
fetch_pc1600_calc old PC1600-P1-B3B            0bfd6f02f6c053c6e9f48554ac40f075
fetch_pc1600_calc old PC1600-P2-B6             2c977fdd8c924c1492a2c23f67a20f23

# CE-1600P peripheral ROMs (independent of the calculator ROM version)
fetch_if_needed "$PC1600_BASE/peripherals/PC1600-P1-B4-CE1600P.BIN"      "$ROMS_DIR/PC1600-P1-B4-CE1600P.bin"      05548a8dda3e572d50d4bd281a650ea8
fetch_if_needed "$PC1600_BASE/peripherals/PC1600-P1-B5-CE1600P-OR-F.BIN" "$ROMS_DIR/PC1600-P1-B5-CE1600P-OR-F.bin" a675c6dbdf7dc4c10e8d96891e196f8f

# Obsolete unsuffixed names from before the new/old split: remove so a stale
# copy can never mask a missing versioned file.
for f in LH5803-C000-FFFF P0-B0 P1-B0 P1-B3 P1-B3B P2-B6; do
  rm -f "$ROMS_DIR/PC1600-$f.bin"
done

# ---- CE-158 (Jeff-Birt/Sharp_CE-158) ----
fetch_if_needed "https://raw.githubusercontent.com/Jeff-Birt/Sharp_CE-158/main/CE-158_ROM_ORIG.bin" "$ROMS_DIR/CE-158.ROM" aa952878fb29da4844791d95185649ca

# ---- CE-150 (tinue/PC-1500-ROM, dumps/) ----
fetch_if_needed "https://raw.githubusercontent.com/tinue/PC-1500-ROM/main/dumps/CE-150.BIN" "$ROMS_DIR/CE-150.ROM" eb9aa5156c6849890b137799efc50a4b

log "done"
