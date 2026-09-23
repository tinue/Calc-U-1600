#!/bin/sh
# Builds the headless CLI harness. Usage: tools/build_cli.sh && ./headless/pc1500_cli roms/PC-1500_A04.ROM
set -e
cd "$(dirname "$0")/.."
mkdir -p headless
clang++ -std=c++17 -Wall -Wextra -O2 \
  Core/CPU/LH5801/LH5801.cpp \
  Core/Audio/PiezoSampler.cpp \
  Core/PC1500/PC1500Memory.cpp \
  Core/PC1500/PC1500Machine.cpp \
  Core/PC1500/PC1500Keyboard.cpp \
  Core/PC1500/PC1500Display.cpp \
  Core/PC1500/Upd1990ac.cpp \
  Core/PC1500/PresetFile.cpp \
  Core/PC1500/PC1500BasicTyper.cpp \
  Core/PC1500/PC1500BasicLoader.cpp \
  Core/PC1500/PC1500PresetLoader.cpp \
  Core/Display/LcdScreenshot.cpp \
  Core/Basic/BasicBinaryImage.cpp \
  Core/Basic/BasicProgramSource.cpp \
  Core/PC1500/PC1500TraceFile.cpp \
  Core/Serial/PtySerialLink.cpp \
  tools/pc1500_cli.cpp \
  Core/Basic/vendor/sharpdx/libsharpdx.a \
  -o headless/pc1500_cli
