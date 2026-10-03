#pragma once
#include <array>
#include <cstdint>

#include "../Display/StatusLine.hpp"

class PC1500Memory;

// ── PC-1500/1500A dot-matrix LCD ─────────────────────────────────────────
//
// 156 x 7 pixel dot-matrix display plus a row of fixed status icons.
// Decodes from a snapshot of PC1500Memory's display-RAM region
// (0x7000-0x77FF's 512-byte backing store, already backed since Phase 1)
// — no new memory-map work, just a read-only interpretation layer on top
// of it.
//
// This is a genuine value-type snapshot, copied once at construction, not
// a live view: PC1500Machine::display() is meant to be safely readable
// from a UI thread after PC1500Machine's internal lock has already been
// released (the lock only needs to cover the copy itself, not every pixel
// read the caller makes afterward). Holding a live `const PC1500Memory&`
// instead would race with the CPU thread's ongoing writes to display RAM
// for every pixel/icon read made after construction.
class PC1500Display {
public:
    static constexpr int kCols = 156;
    static constexpr int kRows = 7;

    /// Copies the display-RAM snapshot out of `memory` immediately.
    explicit PC1500Display(const PC1500Memory& memory);

    /// True if the dot at (col, row) is lit. col: 0-155, row: 0-6 (top to
    /// bottom). Decoded from this object's own snapshot -- cheap (a
    /// handful of byte lookups), immutable once constructed.
    bool pixel(int col, int row) const;

    /// The status line, decoded from the two fixed bytes at 0x764E and
    /// 0x764F right after the 156x7 pixel data -- the same two sets as the
    /// PC-1600's; see Core/Display/StatusLine.hpp. S, ROMAJI, CTRL and BATT
    /// have no bits here and stay off.
    StatusLine statusLine() const;

private:
    // 512-byte copy of PC1500Memory's display-RAM backing store
    // (0x7000-0x77FF's window mirrors onto this size -- see class doc
    // comment above). Indexed via `at(addr)`, which reproduces
    // PC1500Memory's own `(addr - 0x7000) % 0x200` mirroring formula.
    std::array<uint8_t, 0x200> m_bytes;
    uint8_t at(uint16_t addr) const;
};
