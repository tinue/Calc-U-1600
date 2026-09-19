#!/bin/sh
# Builds the headless PC-1600 + CE-1600P plotter probe. Boots a
# `plotter: ce1600p` preset, runs its script, and dumps the plotter
# mechanism's pen/motor/colour events per preset step. Investigation aid
# for the "pen colour drifts out of sync on OFF/ON" issue -- see
# MinorIssues.md.
#
# Usage:
#   tools/build_pc1600_plotter_probe.sh && \
#     ./headless/pc1600_plotter_probe examples/plotter_test.pc1600
set -e
cd "$(dirname "$0")/.."
mkdir -p headless
clang++ -std=c++17 -Wall -Wextra -O1 \
  Core/CPU/LH5801/LH5801.cpp \
  Core/Audio/PiezoSampler.cpp \
  Core/PC1500/PC1500Memory.cpp \
  Core/PC1500/PC1500Machine.cpp \
  Core/PC1500/PC1500Keyboard.cpp \
  Core/PC1500/PC1500Display.cpp \
  Core/PC1500/Upd1990ac.cpp \
  Core/PC1500/PresetFile.cpp \
  Core/PC1500/PC1500BasicTyper.cpp \
  Core/PC1500/PC1500PresetLoader.cpp \
  Core/PC1500/PC1500TraceFile.cpp \
  Core/PC1600/PC1600Memory.cpp \
  Core/PC1600/PC1600Keyboard.cpp \
  Core/PC1600/PC1600Display.cpp \
  Core/PC1600/PC1600SubCpu.cpp \
  Core/PC1600/TC8576F.cpp \
  Core/CPU/SC7852/SC7852.cpp \
  Core/CPU/LH5803/LH5803Memory.cpp \
  Core/CPU/LH5803/LH5803SharedMemory.cpp \
  Core/PC1600/PC1600Machine.cpp \
  Core/PC1600/PC1600BasicTyper.cpp \
  Core/PC1600/PC1600MachineImage.cpp \
  Core/PC1600/PC1600PresetLoader.cpp \
  tools/pc1600_plotter_probe.cpp \
  -o headless/pc1600_plotter_probe
