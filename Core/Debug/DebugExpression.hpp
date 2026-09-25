#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

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

/// An expression parsed once, to be run many times (breakpoint conditions,
/// logpoint parts). Running it gives exactly what evaluate() gives for the
/// same text and context -- the same value, or the same first error:
/// syntax errors are kept in the program at the point where they occurred,
/// so an error from evaluating something before them still wins.
class CompiledExpression {
public:
    static CompiledExpression compile(const std::string& text);
    ExpressionResult run(const ExpressionContext& ctx) const;

private:
    enum class Code : uint8_t { Const, Name, Mem, Unary, Binary, Fail };
    struct Op {
        Code code = Code::Const;
        char sub = 0;       ///< Unary: ! ~ - ; Binary: the operator (see compile()); Mem: 'b', 'w' or '#'
        int64_t value = 0;  ///< Const; Name / Fail: index into m_strings
    };
    friend class ExpressionCompiler;
    std::vector<Op> m_ops;
    std::vector<std::string> m_strings; ///< Name: the name as written, then lower-cased; Fail: the message
};

/// compile(text).run(ctx).
ExpressionResult evaluate(const std::string& text, const ExpressionContext& ctx);

} // namespace debug
