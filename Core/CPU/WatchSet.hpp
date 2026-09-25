#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

// ── Data breakpoints (memory watches) ────────────────────────────────────
//
// A CPU holding a non-null WatchSet* checks every *data* access of an
// instruction against it -- operand reads/writes, stack pushes and pops,
// vector-table reads -- but never its opcode and operand fetches, which go
// through the core's fetch helpers instead. A hit only latches: the
// instruction completes (post-execution semantics, like the history ring)
// and the machine stops after it. Owned by the debugger; a CPU with no
// watches holds nullptr and pays one pointer test per access, one with
// watches a bit test (a 64K-bit map per space and access kind in use).

struct WatchHit {
    uint16_t addr = 0;
    uint8_t  value = 0;   ///< the byte read, or the byte written
    bool     write = false;
    uint8_t  space = 0;   ///< 0 = ME0 / Z-80 memory, 1 = LH580x ME1
};

class WatchSet {
public:
    struct Watch {
        uint16_t lo = 0, hi = 0; ///< inclusive range
        uint8_t  space = 0;      ///< as WatchHit::space
        bool     read = false, write = false;
    };

    void clear() {
        m_watches.clear();
        for (auto& perSpace : m_bits)
            for (auto& map : perSpace) map.reset();
    }
    /// A range with lo > hi (one that wrapped past FFFF) never matches.
    void add(const Watch& w) {
        m_watches.push_back(w);
        if (w.space > 1 || w.lo > w.hi) return;
        for (int write = 0; write < 2; write++) {
            if (!(write ? w.write : w.read)) continue;
            std::unique_ptr<Bits>& map = m_bits[w.space][write];
            if (!map) map = std::make_unique<Bits>();
            for (uint32_t a = w.lo; a <= w.hi; a++) (*map)[a >> 6] |= uint64_t(1) << (a & 63);
        }
    }
    bool empty() const { return m_watches.empty(); }
    const std::vector<Watch>& watches() const { return m_watches; }

    /// Latches the first hit until consumeHit(); later hits in the same
    /// instruction are ignored.
    void check(uint16_t addr, uint8_t value, bool write, uint8_t space) {
        if (m_hitPending || space > 1) return;
        const Bits* map = m_bits[space][write ? 1 : 0].get();
        if (!map || !(((*map)[addr >> 6] >> (addr & 63)) & 1)) return;
        m_hit = {addr, value, write, space};
        m_hitPending = true;
    }
    bool hitPending() const { return m_hitPending; }
    WatchHit consumeHit() { m_hitPending = false; return m_hit; }

private:
    using Bits = std::array<uint64_t, 1024>;

    std::vector<Watch> m_watches;
    std::unique_ptr<Bits> m_bits[2][2]; // [space][write]; null while no watch needs it
    WatchHit m_hit;
    bool m_hitPending = false;
};
