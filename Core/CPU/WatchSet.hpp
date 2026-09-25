#pragma once

#include <cstdint>
#include <vector>

// ── Data breakpoints (memory watches) ────────────────────────────────────
//
// A CPU holding a non-null WatchSet* checks every *data* access of an
// instruction against it -- operand reads/writes, stack pushes and pops,
// vector-table reads -- but never its opcode and operand fetches, which go
// through the core's fetch helpers instead. A hit only latches: the
// instruction completes (post-execution semantics, like the history ring)
// and the machine stops after it. Owned by the debugger; a CPU with no
// watches holds nullptr and pays one pointer test per access.

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

    void clear() { m_watches.clear(); }
    void add(const Watch& w) { m_watches.push_back(w); }
    bool empty() const { return m_watches.empty(); }
    const std::vector<Watch>& watches() const { return m_watches; }

    /// Latches the first hit until consumeHit(); later hits in the same
    /// instruction are ignored.
    void check(uint16_t addr, uint8_t value, bool write, uint8_t space) {
        if (m_hitPending) return;
        for (const Watch& w : m_watches) {
            if (w.space == space && addr >= w.lo && addr <= w.hi && (write ? w.write : w.read)) {
                m_hit = {addr, value, write, space};
                m_hitPending = true;
                return;
            }
        }
    }
    bool hitPending() const { return m_hitPending; }
    WatchHit consumeHit() { m_hitPending = false; return m_hit; }

private:
    std::vector<Watch> m_watches;
    WatchHit m_hit;
    bool m_hitPending = false;
};
