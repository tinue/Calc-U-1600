#pragma once
#include "../LH5801/LH5801.hpp"

// ── LH5803 CPU core ──────────────────────────────────────────────────────
//
// The LH5803 is the PC-1600's LH5801-compatible co-processor. Per the
// Sharp Technical Reference Manual (§7.1.2), it "supports the LH-5801
// instruction set" with no documented ISA delta, so every opcode/flag/
// timing behavior is inherited from `Core/CPU/LH5801` unchanged.
//
// The documented differences are pin-level (reversed address byte order
// and inverted A15 seen from the SC7852 side, ME0/ME1 on pins 29/30, PV
// on pin 60). They don't show at the logical-address level this core
// works at: LH5803Memory / LH5803SharedMemory implement LH5801Bus against
// logical addresses, as PC1500Memory does for the LH5801.
//
// A subclass rather than an alias, so an LH5803-specific difference has
// a place to go.
class LH5803 : public LH5801 {
public:
    using LH5801::LH5801;
};
