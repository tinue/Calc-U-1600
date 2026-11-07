#pragma once
#include "../LH5801/LH5801.hpp"

// ── LH5803 CPU core ──────────────────────────────────────────────────────
//
// The LH5803 is the PC-1600's LH5801-compatible co-processor. Per the
// Sharp Technical Reference Manual (§7.1.2), it "supports almost all
// LH-5801 machine language instructions"; every opcode/flag/timing
// behavior is inherited from `Core/CPU/LH5801` except the three below.
//
// The documented differences are pin-level (reversed address byte order
// and inverted A15 seen from the SC7852 side, ME0/ME1 on pins 29/30, PV
// on pin 60). They don't show at the logical-address level this core
// works at: LH5803Memory / LH5803SharedMemory implement LH5801Bus against
// logical addresses, as PC1500Memory does for the LH5801.
//
// The one documented instruction difference (TRM §7.1.2): SDP, RDP and
// OFF "operate as a NOP instruction in LH-5803" -- a PC-1500 program's OFF
// does not power anything down.
class LH5803 : public LH5801 {
public:
    explicit LH5803(LH5801Bus& bus) : LH5801(bus) { m_lh5803Variant = true; }
};
