#pragma once

#include "Disassembly.hpp"

// ── LH5801 / LH5803 disassembler ─────────────────────────────────────────
//
// One decoder for both CPUs: the LH5803 executes the LH5801 instruction
// set unchanged. Output uses sdaslh5801 syntax -- lower-case mnemonics,
// `(x)` for ME0 and `#(x)` for ME1, `0x` hex -- so a disassembly reads like
// the listings of the project's own sources. The opcode map is the Sharp
// LH5801 instruction set (Technical Reference Manual, as transcribed in
// SharpPC1500Reference's LH5801_Guide.md), matching Core/CPU/LH5801's
// execute()/executeFD() one-for-one. Code is always fetched from ME0.
namespace disasm {

/// Decodes the instruction at `addr`. Never fails: an undocumented opcode
/// comes back `illegal` as a `.db` of the bytes the CPU core consumes for
/// it (one, or two after an FD prefix).
Decoded decodeLH5801(uint16_t addr, const FetchFn& fetch, const SymbolFn& symbols = {});

} // namespace disasm
