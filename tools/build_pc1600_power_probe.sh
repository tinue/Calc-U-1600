#!/bin/sh
# Builds the throw-away OFF-key ROM-path probe (see
# tools/pc1600_power_probe.cpp and docs/PC1600-Power-Button-Plan.md).
# -DPC1600_POWER_PROBE lights up the instrumentation hooks in
# PC1600Memory::writeIO / PC1600SubCpu::command; no other build defines it.
#
#   tools/build_pc1600_power_probe.sh && ./headless/pc1600_power_probe roms
set -e
cd "$(dirname "$0")/.."
mkdir -p headless
clang++ -std=c++17 -Wall -Wextra -O2 -DPC1600_POWER_PROBE \
  Core/CPU/LH5801/LH5801.cpp \
  Core/CPU/SC7852/SC7852.cpp \
  Core/CPU/LH5803/LH5803Memory.cpp \
  Core/CPU/LH5803/LH5803SharedMemory.cpp \
  Core/PC1500/PC1500TraceFile.cpp \
  Core/PC1600/PC1600Memory.cpp \
  Core/PC1600/PC1600Keyboard.cpp \
  Core/PC1600/PC1600Display.cpp \
  Core/PC1600/PC1600SubCpu.cpp \
  Core/PC1600/TC8576F.cpp \
  Core/PC1600/PC1600Machine.cpp \
  tools/pc1600_power_probe.cpp \
  -o headless/pc1600_power_probe
