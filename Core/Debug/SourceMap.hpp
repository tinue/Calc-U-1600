#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "DebugTarget.hpp"
#include "Listing/Listing.hpp"

// ── Source map ───────────────────────────────────────────────────────────
//
// Which source line an address belongs to, and the reverse, across all
// listings the debugger knows. A listing is bound to one CPU (thread) and
// optionally a bank qualifier, in one of two ways:
//
//  - loaded: it came with a program the debugger just loaded, and owns
//    exactly the loaded range. A newer load replaces the loaded bindings
//    its range overlaps -- the emulator knows which listing matches which
//    bytes because it put them there.
//  - static: attached as-is (typically ROM listings), in attach order.
//
// Lookups prefer loaded bindings (newest first), then static ones. A
// binding whose bytes don't match memory is stale and takes no part.

namespace debug {

/// Whether `key` matches the machine's state for `addr` on `thread` now
/// (see DebugTarget::bankMatches()).
using BankMatch = std::function<bool(int thread, const BankKey& key, uint16_t addr)>;
/// A side-effect-free code byte read; false if unreadable.
using CodePeek = std::function<bool(int thread, uint16_t addr, uint8_t* value)>;

struct SourceLocation {
    std::string file; ///< absolute path
    int line = 0;     ///< 1-based
    int binding = 0;
};

class SourceMap {
public:
    /// Lookup tables of one binding, built when it is added.
    struct Index {
        std::map<uint16_t, size_t> byAddr;                  // line start -> index into listing.lines
        std::map<int, std::map<int, uint16_t>> byFileLine;  // file index -> line -> first address
        std::map<uint16_t, std::string> labels;             // value -> name
    };
    struct Binding {
        int id = 0;
        int thread = 1;
        BankKey key;
        bool loaded = false;
        uint16_t lo = 0, hi = 0xFFFF; ///< inclusive; the loaded range (static: everything)
        std::string name;             ///< the listing path, for messages
        Listing listing;
        bool stale = false;
        int checked = 0, mismatched = 0; ///< last verification
        Index index;
    };

    int addStatic(int thread, Listing listing, const BankKey& key, const std::string& name);
    /// Replaces the loaded bindings of `thread` whose range overlaps [lo, hi].
    int addLoaded(int thread, Listing listing, const BankKey& key, uint16_t lo, uint16_t hi, const std::string& name);
    void clear() { m_bindings.clear(); m_nextId = 1; }
    void remove(int id);

    const Binding* binding(int id) const;
    const std::vector<Binding>& bindings() const { return m_bindings; }

    /// Compares each listing line's bytes (within the binding's range, and
    /// only where its bank qualifier holds right now) with memory. Any
    /// difference marks the binding stale. Returns the mismatching lines.
    int verify(int id, const BankMatch& match, const CodePeek& peek);

    /// The source line of the instruction at `addr`. With `peek`, the
    /// line's bytes must also still be in memory (else false: show the
    /// disassembly instead).
    bool lookup(int thread, uint16_t addr, const BankMatch& match, SourceLocation* out,
                const CodePeek& peek = {}) const;

    struct CodeAddress {
        int thread = 1;
        uint16_t addr = 0;
        BankKey key;
        int binding = 0;
    };
    /// Addresses of `file`:`line`, or of the next line below it that has
    /// code; *resolvedLine receives that line (0 if none).
    std::vector<CodeAddress> addressesFor(const std::string& file, int line, int* resolvedLine) const;

    /// Whether any live binding has code from `file`.
    bool knowsFile(const std::string& file) const;

    /// A symbol and the binding that defines it: loaded bindings newest
    /// first, then static ones; stale bindings don't count.
    struct SymbolInfo {
        uint16_t value = 0;
        int thread = 1;
        BankKey key;
        int binding = 0;
    };
    bool findSymbol(const std::string& name, SymbolInfo* info) const;
    /// findSymbol()'s value.
    bool symbolValue(const std::string& name, uint16_t* value) const;
    /// A label whose value is exactly `addr` in a binding of `thread`, or "".
    std::string symbolAt(int thread, uint16_t addr) const;

private:
    int add(Binding b);
    /// Calls `f` on each binding in lookup order -- loaded ones newest
    /// first, then static ones -- until it returns true.
    template <typename F>
    bool firstOf(F f) const {
        for (size_t i = m_bindings.size(); i-- > 0;)
            if (m_bindings[i].loaded && f(m_bindings[i])) return true;
        for (const Binding& b : m_bindings)
            if (!b.loaded && f(b)) return true;
        return false;
    }

    std::vector<Binding> m_bindings;
    int m_nextId = 1;
};

} // namespace debug
