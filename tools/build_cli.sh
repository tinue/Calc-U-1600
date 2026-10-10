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
  Core/Debug/Inspect/Inspector.cpp \
  Core/Debug/Inspect/TextTable.cpp \
  Core/Debug/Inspect/HexDump.cpp \
  Core/Debug/Inspect/PC1500Inspector.cpp \
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
  Core/Serial/PtySerialLink.cpp \
  tools/pc1500_cli.cpp \
  Core/Basic/vendor/sharpdx/libsharpdx.a \
  -o headless/pc1500_cli
