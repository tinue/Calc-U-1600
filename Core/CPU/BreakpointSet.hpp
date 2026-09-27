#pragma once

#include <cstdint>

#include "AddressBits.hpp"

// PC breakpoints of one CPU: a 64K-bit map, so the per-instruction check is
// one bit test. Single-threaded -- the debugger edits it from the thread
// that runs the machine, between steps (see HistoryRing.hpp).
class BreakpointSet {
public:
    void add(uint16_t addr) { m_bits.add(addr); }
    void remove(uint16_t addr) { m_bits.remove(addr); }
    void clear() { m_bits.clear(); }
    bool contains(uint16_t addr) const { return m_bits.contains(addr); }

    /// True (and latches a hit for consumeHit()) if `pc` is a breakpoint.
    bool check(uint16_t pc) {
        if (!contains(pc)) return false;
        m_hit = true;
        return true;
    }
    /// Returns true once per hit.
    bool consumeHit() {
        const bool hit = m_hit;
        m_hit = false;
        return hit;
    }
    void clearHit() { m_hit = false; }

private:
    AddressBits m_bits;
    bool m_hit = false;
};
