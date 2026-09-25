#pragma once

#include <array>
#include <cstdint>

// PC breakpoints of one CPU: a 64K-bit map, so the per-instruction check is
// one bit test. Single-threaded -- the debugger edits it from the thread
// that runs the machine, between steps (see HistoryRing.hpp).
class BreakpointSet {
public:
    void add(uint16_t addr) { m_bits[addr >> 6] |= bit(addr); }
    void remove(uint16_t addr) { m_bits[addr >> 6] &= ~bit(addr); }
    void clear() { m_bits.fill(0); }
    bool contains(uint16_t addr) const { return (m_bits[addr >> 6] & bit(addr)) != 0; }

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
    static uint64_t bit(uint16_t addr) { return uint64_t(1) << (addr & 63); }

    std::array<uint64_t, 1024> m_bits{};
    bool m_hit = false;
};
