#pragma once

#include <cstdint>
#include <functional>
#include <string>

// ── Shared disassembler types ────────────────────────────────────────────
//
// The debugger's disassemblers (LH5801Disassembler, Z80Disassembler) decode
// one instruction at a time through a caller-supplied byte fetch, so they
// work on live, bank-switched memory through a side-effect-free peek as
// well as on a plain buffer in tests. Besides the text they classify the
// instruction's control flow; step over / step out are built on that.

namespace disasm {

/// Reads one byte of code at `addr` -- must have no side effects.
using FetchFn = std::function<uint8_t(uint16_t addr)>;

/// Optional label lookup: the symbol for `addr`, or empty if none. Used to
/// print branch/call targets and absolute operands as names.
using SymbolFn = std::function<std::string(uint16_t addr)>;

enum class Flow : uint8_t {
    None,      ///< falls through to the next instruction
    Jump,      ///< unconditional transfer; `target` valid unless `indirect`
    CondJump,  ///< conditional transfer (branch, loop) or fall through
    Call,      ///< pushes a return address, then transfers (SJP, CALL, RST)
    CondCall,  ///< conditional Call (CALL cc, the LH5801's VCS/VZR/...)
    Vector,    ///< LH5801 VEJ/VMJ: a Call through the vector table at FFxx
    Return,    ///< pops the return address (RTN, RTI, RET, RETI, RETN)
    CondReturn,///< RET cc
    Halt,      ///< HLT / HALT / OFF: stops until an interrupt (or power on)
};

struct Decoded {
    std::string text;       ///< e.g. "lda (0x7A00)" -- assembler syntax of the toolchain
    uint8_t     len = 1;    ///< instruction length in bytes, >= 1
    Flow        flow = Flow::None;
    bool        hasTarget = false; ///< `target` is a known static address
    uint16_t    target = 0;        ///< branch/jump/call target (Vector: the resolved handler)
    bool        illegal = false;   ///< not a documented opcode; `text` is a data byte
};

/// "0x1F" / "0x7A00" -- the operand style both assemblers accept.
std::string hex8(uint8_t v);
std::string hex16(uint16_t v);

/// The symbol for `addr` if `symbols` knows one, else hex16(addr).
std::string addrText(uint16_t addr, const SymbolFn& symbols);

} // namespace disasm
