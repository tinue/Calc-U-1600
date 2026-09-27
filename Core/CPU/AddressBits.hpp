#pragma once

#include <array>
#include <cstdint>

// A 64K-bit address map: one bit test per lookup. Shared by the PC
// breakpoints (BreakpointSet) and the memory watches (WatchSet).
class AddressBits {
public:
    void add(uint16_t addr) { m_bits[addr >> 6] |= bit(addr); }
    /// Inclusive; nothing when lo > hi.
    void addRange(uint16_t lo, uint16_t hi) {
        for (uint32_t a = lo; a <= hi; a++) add(uint16_t(a));
    }
    void remove(uint16_t addr) { m_bits[addr >> 6] &= ~bit(addr); }
    void clear() { m_bits.fill(0); }
    bool contains(uint16_t addr) const { return (m_bits[addr >> 6] & bit(addr)) != 0; }

private:
    static uint64_t bit(uint16_t addr) { return uint64_t(1) << (addr & 63); }

    std::array<uint64_t, 1024> m_bits{};
};
