#!/bin/sh
# Builds the headless PC-1600 CLI harness. Usage:
#   tools/build_pc1600_cli.sh && ./headless/pc1600_cli roms/PC1600-P0-B0-new.bin roms/PC1600-P1-B0-new.bin
#   tools/build_pc1600_cli.sh && ./headless/pc1600_cli --preset examples/foo.pc1600 --dump-basic
set -e
cd "$(dirname "$0")/.."
mkdir -p headless
clang++ -std=c++17 -Wall -Wextra -O2 \
  Core/CPU/LH5801/LH5801.cpp \
  Core/CPU/SC7852/SC7852.cpp \
  Core/CPU/LH5803/LH5803Memory.cpp \
  Core/CPU/LH5803/LH5803SharedMemory.cpp \
  Core/Preset/PresetFile.cpp \
  Core/Preset/PresetRunner.cpp \
  Core/PC1500/PC1500Keyboard.cpp \
  Core/PC1500/PC1500TraceFile.cpp \
  Core/Audio/PiezoSampler.cpp \
  Core/PC1600/PC1600Memory.cpp \
  Core/PC1600/PC1600SubCpu.cpp \
  Core/PC1600/TC8576F.cpp \
  Core/Serial/PtySerialLink.cpp \
  Core/PC1600/PC1600Display.cpp \
  Core/PC1600/PC1600Keyboard.cpp \
  Core/PC1600/PC1600Machine.cpp \
  Core/PC1600/PC1600BasicTyper.cpp \
  Core/PC1600/PC1600BasicLoader.cpp \
  Core/PC1600/PC1600ProgramPlacement.cpp \
  Core/PC1600/PC1600MachineImage.cpp \
  Core/PC1600/PC1600MachineCodeLoader.cpp \
  Core/PC1600/PC1600PresetLoader.cpp \
  Core/Display/LcdScreenshot.cpp \
  Core/Basic/BasicBinaryImage.cpp \
  Core/Basic/BasicProgramSource.cpp \
  tools/pc1600_cli.cpp \
  Core/Basic/vendor/sharpdx/libsharpdx.a \
  -o headless/pc1600_cli
