#pragma once

#include <cstdint>
#include <functional>
#include <string>

// ── Debugger expressions ─────────────────────────────────────────────────
//
// A small C-like integer expression language for breakpoint conditions and
// hit counts, logpoint `{expr}` interpolation, watch/hover/REPL evaluation
// and register edits:
//
//   literals   0x7A00  $7A00  &7A00 (hex)   42 (decimal)
//   names      registers and flags of the CPU (case-insensitive), then
//              symbols from the loaded listings
//   memory     [addr] byte, w[addr] word in the CPU's byte order,
//              #[addr] byte from the LH580x's ME1
//   operators  ! ~ - (unary)  * / %  + -  << >>  < <= > >=  == !=  &  ^  |
//              &&  ||  and parentheses, with C precedence
//
// Values are 64-bit signed; comparisons and logic yield 0 or 1.

namespace debug {

struct ExpressionContext {
    /// A register, flag or symbol by (lower-case) name.
    std::function<bool(const std::string& name, int64_t* value)> lookup;
    /// A byte without side effects; false if unreadable. `me1` only for #[].
    std::function<bool(uint16_t addr, bool me1, uint8_t* value)> readByte;
    bool bigEndian = false; ///< LH580x words are high byte first; the Z-80's low byte first
};

struct ExpressionResult {
    bool ok = false;
    int64_t value = 0;
    std::string error; ///< set when !ok, e.g. "unknown name 'foo'"
};

ExpressionResult evaluate(const std::string& text, const ExpressionContext& ctx);

} // namespace debug
