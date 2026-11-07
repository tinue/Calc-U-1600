#pragma once
#include <cstdint>
#include <functional>

#include "../PC1600/PC1600ProgramPlacement.hpp"

namespace CoreDebug {

// ── PC-1600 program areas (S0, and S1/S2 program modules) ────────────────
//
// What the ROM itself reports, computed the ROM's way so the inspector
// (Core/Debug/Inspect) agrees with MEM / STATUS 259 / STATUS 260. Sources:
// Ref/PC-1600/PC-1600-Work-Area-Map.md §3.5 and §4.5 (LH5803 ROM $CC30 / $CE41).
struct PC1600SlotProgramArea : pc1600::SlotDescriptor {
    int freeBytes = 0;           // STATUS 259 / 260
};

struct PC1600ProgramAreas {
    uint8_t title = 0;           // F1D5H: 0 = S0, 1 = S1, 2 = S2
    uint8_t startIndex = 0;      // F02BH: ADTBL index of the S0 program start
    uint8_t endIndex = 0;        // F02CH: ADTBL index of the S0 program end
    uint8_t ramEndPage = 0;      // F864H
    int memS0 = 0;               // MEM / STATUS 0 (always S0, whatever TITLE says)
    PC1600SlotProgramArea slot[2];  // S1, S2
};

PC1600ProgramAreas readPC1600ProgramAreas(const std::function<uint8_t(uint16_t)>& peek);

}  // namespace CoreDebug
