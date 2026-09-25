#pragma once
#include <cstdint>

// ── Trace feature flags ────────────────────────────────────────────────────
//
// Independently gated cost tiers for the instruction trace / debug API.
// Set via LH5801::setTraceFlags() or PC1500Machine::setTraceFlags().
// The PC-1600's two CPUs each carry their own flags/ring, told apart by
// the CPU-id tag below.

enum : uint32_t {
    TRACE_NONE        = 0x0000,
    TRACE_PC          = 0x0001,  ///< (seqno, pc, opcode, cycles) only — cheapest tier
    TRACE_REGS_LIGHT  = 0x0002,  ///< adds A, XL/XH, YL/YH, UL/UH, S, T — all general registers
    TRACE_REGS_FULL   = 0x0004,  ///< adds PU, PV, DISP, TM — full CPU state
    TRACE_BREAKPOINTS = 0x0008,  ///< breakpoint-list check on every step(); stop-the-world
    /// Every register tier: what a file capture (GUI TRACE, preset `trace:`) records.
    TRACE_FULL        = TRACE_PC | TRACE_REGS_LIGHT | TRACE_REGS_FULL,
};

// ── CPU-id tag ─────────────────────────────────────────────────────────────
//
// Lets frames from the PC-1600's two independent CPU trace rings be told
// apart once merged into one stream at the GUI/tooling layer -- each CPU
// still carries its own ring; this tag is not used to route
// anything at the Core level. Defaults to UNSPECIFIED for the PC-1500/
// 1500A's single LH5801 (which has no PC-1600-style sibling CPU to
// distinguish itself from) and for LH5803 unless PC1600Machine explicitly
// tags it via LH5801::setCpuIdTag().

enum : uint8_t {
    CPU_ID_UNSPECIFIED = 0,
    CPU_ID_SC7852      = 1,
    CPU_ID_LH5803      = 2,
};

// ── CPU trace frame (LH5801 / LH5803) ─────────────────────────────────────
//
// One entry per executed instruction, captured into a lock-free ring buffer.
// Field groups populated depend on which TRACE_* flags are active at the time
// of capture (see LH5801::step()).

struct CpuFrame {
    // Identity (always captured when tracing is enabled at all)
    uint32_t seqno{};
    uint16_t pc{};       ///< P at the start of this instruction (before fetch)
    uint16_t opcode{};   ///< first opcode byte; if FD-prefixed, 0xFD00 | secondByte
    uint8_t  cycles{};   ///< cycle weight consumed by this instruction
    uint8_t  cpuId{CPU_ID_UNSPECIFIED};

    // TRACE_REGS_LIGHT
    uint8_t  a{};
    uint8_t  xl{}, xh{}, yl{}, yh{}, ul{}, uh{};
    uint16_t s{};
    uint8_t  t{};        ///< status register: bit0=C,1=IE,2=Z,3=V,4=H

    // TRACE_REGS_FULL
    uint8_t  pu{}, pv{}, disp{};
    uint16_t tm{};
};

// ── CPU trace frame (SC7852) ──────────────────────────────────────────────
//
// Same ring-buffer/TRACE_* gating convention as CpuFrame, but shaped for
// the SC7852's Z-80 register set instead of the LH5801's.

struct Z80CpuFrame {
    uint32_t seqno{};
    uint16_t pc{};
    uint16_t opcode{};   ///< first opcode byte; 0xCBxx/0xEDxx/0xDDxx/0xFDxx for prefixed forms
    uint8_t  cycles{};
    uint8_t  cpuId{CPU_ID_SC7852};

    // TRACE_REGS_LIGHT
    uint16_t af{}, bc{}, de{}, hl{}, ix{}, iy{}, sp{};

    // TRACE_REGS_FULL
    uint8_t  i{}, r{};
    bool     iff1{}, iff2{};
    uint8_t  im{};
};
