#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "../MachineCodeFile.hpp"
#include "PC1600ProgramPlacement.hpp"

class PC1600Machine;

// Writes a machine-language block linearly into one PC-1600 load target:
// `slot` 0 = S0, the internal RAM ($C000-$FFFF); 1 / 2 = the memory slots'
// $8000-$BFFF window. `addr` is the Z-80 (SC7852) address -- no +$8000
// conversion. The bytes go straight into the backing store
// (debugWriteInternalRam / debugWriteSlotImage), so the current bank
// state doesn't matter. Shared by the preset loader's `format: binary`
// and the GUI's "Load Machine Code…". Returns false, writing nothing, with
// `error` set (no "section N:" prefix -- the caller adds its own context).
bool loadPC1600MachineCode(PC1600Machine& machine, int slot, uint32_t addr, const uint8_t* data, size_t len,
                           std::string* error);

// Geometry of the module in `slot` (1 or 2) as the program-placement logic
// needs it (PC1600ProgramPlacement.hpp). Shared with the fast BASIC loader.
pc1600::SlotGeometry pc1600SlotGeometry(PC1600Machine& machine, int slot);

// The live BASIC program area ("S0") as machinecode::BasicArea runs, in
// ADTBL order -- the first is where `NEW "S0:",<size>` reserves from. Read
// from the work area (S0MTb / ADTBL / BASPRG_ST) via the placement logic;
// empty if that can't be read (e.g. a machine that hasn't booted).
std::vector<machinecode::BasicArea> pc1600BasicAreas(PC1600Machine& machine);
