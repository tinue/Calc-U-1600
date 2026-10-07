#pragma once
#include <cstdint>

#include "ExpansionCard.hpp"
#include "SystemBusCard.hpp"

// What the PC-1500/1500A puts on its connector contacts. The 40-pin plug
// (ExpansionConnector) carries the TC40H139F (Y0-Y3) / TC40H138F (S0-S7)
// chip selects, identical on both models -- see
// Ref/PC-1500/Memory-Architecture/PC-1500-Address-Decoding.md §2 and
// Ref/Shared/Expansion-Connectors.md §3.1; only which S-strobe lands on which
// contact differs per model (ExpansionConnector::sBlockPin). The 60-pin plug
// (SystemBus) carries none of them: see systemBusPins().
namespace PC1500SignalDecode {

inline bool isY0(uint16_t addr) { return addr < 0x4000; }
inline bool isY2(uint16_t addr) { return addr >= 0x8000 && addr < 0xC000; }

// Returns 1-5 for S1-S5 (2KB blocks &4800-&6FFF), or 0 if addr isn't in any
// S-block a connector ever routes out. S0 (&4000-&47FF) is always built-in
// RAM and never reaches a connector pin; S6/S7 (&7000-&7FFF, display/system
// RAM) likewise have no external strobe wired to either connector
// (Ref/Shared/Expansion-Connectors.md §2.1/§3.2 -- only S1-S5 ever appear on a pin, and
// S5 only reaches the PC-1500A's 40-pin connector, never the PC-1500's).
inline int sBlockIndex(uint16_t addr) {
    if (addr < 0x4800 || addr >= 0x7000) return 0;
    return 1 + int((addr - 0x4800) >> 11); // 0x800 (2KB) per S-block
}

// The address/forWrite plus the PU/PV (pin 2/3) and Y0/Y2 (pin 4/19)
// portion of a 40-pin PinState; the S-block strobe is left to
// ExpansionConnector::decode().
// PU/PV as measured on a real PC-1500: contact 2 = PU, 3 = PV (both PC-1500
// TRM tables print them swapped; Ref/Shared/Expansion-Connectors.md §2.2b).
inline PinState basePinState(uint16_t addr, bool forWrite, bool pu, bool pv) {
    PinState pins;
    pins.address = addr;
    pins.forWrite = forWrite;
    pins.pin[2] = pu;          // PU
    pins.pin[3] = pv;          // PV
    pins.pin[4] = isY0(addr);  // Y0 chip select, &0000-&3FFF
    pins.pin[19] = isY2(addr); // Y2 chip select, &8000-&BFFF
    return pins;
}

// The 60-pin contacts of one LH5801 cycle (TRM §4-3-2, with 15/16
// corrected by measurement: Ref/Shared/Expansion-Connectors.md §2.2b). The
// 60-pin plug carries no Y/S chip selects: an ME0 cycle asserts DME0, an ME1
// cycle ME1 and DME1. Taken to be the same on the PC-1500A: the research
// has a reassignment table for the 40-pin plug only.
inline SystemBusPins systemBusPins(uint16_t addr, bool forWrite, bool me1, bool pu, bool pv) {
    SystemBusPins pins;
    pins.address = addr;
    pins.forWrite = forWrite;
    pins.pin[Contact60::kPU] = pu;
    pins.pin[Contact60::kPV] = pv;
    if (me1) {
        pins.pin[Contact60::kMe1] = true;
        pins.pin[Contact60::kDme1] = true;
    } else {
        pins.pin[Contact60::kDme0] = true;
    }
    return pins;
}

} // namespace PC1500SignalDecode
