#pragma once
#include <cstdint>

#include "ExpansionCard.hpp"

// Shared address decode logic for both the PC-1500/1500A's 40-pin
// (ExpansionConnector) and 60-pin (SystemBus) connectors. The underlying
// TC40H139F (Y0-Y3) / TC40H138F (S0-S7) decoders are identical on both
// models and reach both connectors identically -- see
// SharpPC1500Reference/Memory-Architecture/PC-1500-Address-Decoding.md §2
// and Expansion-Connectors.md §3.1. Only *pin routing* (which of these
// strobes reaches a given physical pin, on a given connector, on a given
// model) differs -- that's each connector class's own concern, not this
// file's.
namespace PC1500SignalDecode {

inline bool isY0(uint16_t addr) { return addr < 0x4000; }
inline bool isY2(uint16_t addr) { return addr >= 0x8000 && addr < 0xC000; }

// Returns 1-5 for S1-S5 (2KB blocks &4800-&6FFF), or 0 if addr isn't in any
// S-block a connector ever routes out. S0 (&4000-&47FF) is always built-in
// RAM and never reaches a connector pin; S6/S7 (&7000-&7FFF, display/system
// RAM) likewise have no external strobe wired to either connector
// (Expansion-Connectors.md §2.1/§3.2 -- only S1-S5 ever appear on a pin, and
// S5 only reaches the PC-1500A's 40-pin connector, never the PC-1500's).
inline int sBlockIndex(uint16_t addr) {
    if (addr < 0x4800 || addr >= 0x7000) return 0;
    return 1 + int((addr - 0x4800) >> 11); // 0x800 (2KB) per S-block
}

// The address/forWrite plus the PU/PV (pin 3/2) and Y0/Y2 (pin 4/19)
// portion of a PinState, common to both connectors' ME0-side decode() --
// the sBlockIndex()-derived S-block strobe is deliberately left to the
// caller, since S-block-to-pin routing is the one thing that actually
// differs between ExpansionConnector and SystemBus (see their decode()).
inline PinState basePinState(uint16_t addr, bool forWrite, bool pu, bool pv) {
    PinState pins;
    pins.address = addr;
    pins.forWrite = forWrite;
    pins.pin[3] = pu;          // PU
    pins.pin[2] = pv;          // PV
    pins.pin[4] = isY0(addr);  // Y0 chip select, &0000-&3FFF
    pins.pin[19] = isY2(addr); // Y2 chip select, &8000-&BFFF
    return pins;
}

} // namespace PC1500SignalDecode
