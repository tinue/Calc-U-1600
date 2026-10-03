#!/bin/sh
# Builds the loader-matrix harness into headless/loader-matrix/.
# Usage: dev/loader-matrix/build.sh
set -e
cd "$(dirname "$0")/../.."
mkdir -p headless/loader-matrix
clang++ -std=c++17 -Wall -Wextra -O2 \
  Core/CPU/LH5801/LH5801.cpp \
  Core/Audio/PiezoSampler.cpp \
  Core/PC1500/PC1500Memory.cpp \
  Core/PC1500/PC1500Machine.cpp \
  Core/PC1500/PC1500Keyboard.cpp \
  Core/PC1500/PC1500Display.cpp \
  Core/PC1500/Upd1990ac.cpp \
  Core/Preset/PresetFile.cpp \
  Core/Preset/PresetRunner.cpp \
  Core/MachineCodeFile.cpp \
  Core/ProgramFile.cpp \
  Core/PC1500/PC1500BasicTyper.cpp \
  Core/PC1500/PC1500BasicLoader.cpp \
  Core/PC1500/PC1500PresetLoader.cpp \
  Core/PC1500/PC1500MachineCodeLoader.cpp \
  Core/Display/LcdScreenshot.cpp \
  Core/Display/LcdText.cpp \
  Core/Basic/BasicProgramSource.cpp \
  Core/PC1500/PC1500TraceFile.cpp \
  dev/loader-matrix/pc1500_loader_matrix.cpp \
  Core/Basic/vendor/sharpdx/libsharpdx.a \
  -o headless/loader-matrix/pc1500_loader_matrix
