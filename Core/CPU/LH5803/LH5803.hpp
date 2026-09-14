#pragma once
#include "../LH5801/LH5801.hpp"

// ── LH5803 CPU core ──────────────────────────────────────────────────────
//
// The LH5803 is the PC-1600's LH5801-compatible co-processor. Per the
// Sharp Technical Reference Manual (§7.1.2), it "supports the LH-5801
// instruction set" with no documented ISA delta, so every opcode/flag/
// timing behavior is inherited from `Core/CPU/LH5801` unchanged.
//
// The documented differences are pin-level facts about how the LH5803
// presents its address bus to the SC7852-side gate array -- reversed
// address byte order and inverted A15 as observed from the SC7852 side,
// plus the ME0 (pin 29)/ME1 (pin 30) memory-enable signals and the PV pin
// (60, the same physical flip-flop as the PC-1500's own PV, relayed
// through the gate array to SC7852's PVOUT). None of these has an
// observable effect at the logical-address level this emulator operates
// at: LH5801Bus already models ME0/ME1 as two logical banks (readME0/
// writeME0/readME1/writeME1) and PV as CPU-visible state (LH5801::pv());
// the physical wire ordering/polarity that carries a given logical address
// or flag value to the mainboard is a hardware-signal detail this core has
// no reason to re-derive, since Core/PC1600/LH5803Memory.hpp implements
// LH5801Bus directly against logical addresses, not raw pins -- the same
// abstraction boundary Core/PC1500/PC1500Memory.hpp draws for the
// standalone LH5801.
//
// Declared as a real (if currently empty) subclass rather than a type
// alias so a genuine LH5803-specific behavioral delta has somewhere to go
// without an interface change.
class LH5803 : public LH5801 {
public:
    using LH5801::LH5801;
};
