#pragma once

#include "Disassembly.hpp"

// ── Z-80 (SC7852) disassembler ───────────────────────────────────────────
//
// Decodes the Zilog Z80 instruction set (Z80 CPU User Manual opcode
// structure: the x/y/z/p/q fields of each opcode byte) including the
// undocumented forms the SC7852 core executes: SLL, IXH/IXL/IYH/IYL
// halves, DD/FD-CB results copied to a register, IN F,(C) / OUT (C),0 and
// the ED duplicates of NEG/RETN/IM. Output is lower-case zasm syntax with
// `0x` hex.
namespace disasm {

/// Decodes the instruction at `addr`. Never fails: an undefined ED opcode
/// comes back `illegal` as a two-byte `.db` (the CPU treats it as a NOP),
/// and a DD/FD directly followed by another prefix as a one-byte `.db`
/// (the CPU drops it and lets the later prefix decide).
Decoded decodeZ80(uint16_t addr, const FetchFn& fetch, const SymbolFn& symbols = {});

} // namespace disasm
