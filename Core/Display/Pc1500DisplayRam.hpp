#pragma once
#include <cstdint>

// ── The PC-1500's display-RAM layout ─────────────────────────────────────
//
// 156 columns x 8 dots (7 shown on the PC-1500) in two blocks of 78
// columns. Each block is split into two halves of 39 columns, at 7600H
// (columns 0-38 / 78-116) and 7700H (39-77 / 117-155). A column is a byte
// pair at base + 2 x local: the first byte holds dots 0-3, the second dots
// 4-7, block 0 in the low nibbles and block 1 in the high ones. 764EH/764FH
// right after the 156 columns are the status-symbol sets 00H/01H
// (Core/Display/StatusLine.hpp).
//
// Used by PC1500Display (reading a PC-1500's screen) and by the PC-1600's
// mirror of LH5803 writes to this RAM onto its LCD (PC1600Display::
// mirrorPc1500Column()).
namespace pc1500ram {

inline constexpr uint16_t kStatusSet0 = 0x764E;
inline constexpr uint16_t kStatusSet1 = 0x764F;

/// True for 7600H-764DH / 7700H-774DH, the 156 columns' byte pairs.
constexpr bool isColumnByte(uint16_t addr) {
    const uint16_t page = addr & 0xFF00;
    return (page == 0x7600 || page == 0x7700) && (addr & 0xFF) < 0x4E;
}

/// The first byte of column `col`'s pair (0-155).
constexpr uint16_t columnPairAddr(int col) {
    const int within = col % 78;
    return uint16_t((within < 39 ? 0x7600 : 0x7700) + (within % 39) * 2);
}

/// The two columns a byte pair at `addr` (either byte) holds: block 0 and
/// block 1.
constexpr int columnOf(uint16_t addr, int block) {
    return block * 78 + ((addr & 0x0100) ? 39 : 0) + ((addr & 0xFF) >> 1);
}

/// Column dots (bit n = dot n, top first) of `block` from its byte pair.
constexpr uint8_t columnDots(uint8_t first, uint8_t second, int block) {
    return block == 0 ? uint8_t((first & 0x0F) | ((second & 0x0F) << 4))
                      : uint8_t((first >> 4) | (second & 0xF0));
}

} // namespace pc1500ram
