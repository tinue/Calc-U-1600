#!/bin/sh
# Builds the PC-1600 loader-matrix harness into headless/loader-matrix/.
# Usage: dev/loader-matrix/build_pc1600.sh
set -e
cd "$(dirname "$0")/../.."
mkdir -p headless/loader-matrix
clang++ -std=c++17 -Wall -Wextra -O2 \
  Core/CPU/LH5801/LH5801.cpp \
  Core/CPU/SC7852/SC7852.cpp \
  Core/CPU/LH5803/LH5803Memory.cpp \
  Core/CPU/LH5803/LH5803SharedMemory.cpp \
  Core/Preset/PresetFile.cpp \
  Core/Preset/PresetRunner.cpp \
  Core/MachineCodeFile.cpp \
  Core/ProgramFile.cpp \
  Core/PC1500/PC1500Keyboard.cpp \
  Core/PC1500/PC1500TraceFile.cpp \
  Core/Audio/PiezoSampler.cpp \
  Core/PC1600/PC1600Memory.cpp \
  Core/PC1600/PC1600SubCpu.cpp \
  Core/PC1600/TC8576F.cpp \
  Core/PC1600/PC1600Display.cpp \
  Core/PC1600/PC1600Keyboard.cpp \
  Core/PC1600/PC1600Machine.cpp \
  Core/PC1600/PC1600BasicTyper.cpp \
  Core/PC1600/PC1600BasicLoader.cpp \
  Core/PC1600/PC1600ProgramPlacement.cpp \
  Core/PC1600/PC1600MachineCodeLoader.cpp \
  Core/PC1600/PC1600PresetLoader.cpp \
  Core/Display/LcdScreenshot.cpp \
  Core/Display/LcdText.cpp \
  Core/Basic/BasicProgramSource.cpp \
  dev/loader-matrix/pc1600_loader_matrix.cpp \
  Core/Basic/vendor/sharpdx/libsharpdx.a \
  -o headless/loader-matrix/pc1600_loader_matrix
