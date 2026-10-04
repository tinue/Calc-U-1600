#include "PC1500Display.hpp"

#include "../Display/Pc1500DisplayRam.hpp"
#include "PC1500Memory.hpp"

PC1500Display::PC1500Display(const PC1500Memory& memory) {
    // memory.displayRamSnapshot() is display RAM's full backing store, and
    // every address this class reads (0x7600-0x764D, 0x7700-0x774D,
    // 0x764E, 0x764F) falls within it -- see PC1500Memory.hpp's memory-map
    // comment for the mirroring behind this.
    m_bytes = memory.displayRamSnapshot();
}

uint8_t PC1500Display::at(uint16_t addr) const {
    return m_bytes[(addr - PC1500Memory::kDisplayRamBase) % m_bytes.size()];
}

bool PC1500Display::pixel(int col, int row) const {
    if (col < 0 || col >= kCols || row < 0 || row >= kRows) return false;
    // Byte-pair layout: Core/Display/Pc1500DisplayRam.hpp.
    const uint16_t addr = pc1500ram::columnPairAddr(col);
    const uint8_t dots = pc1500ram::columnDots(at(addr), at(uint16_t(addr + 1)), col / 78);
    return ((dots >> row) & 1) != 0;
}

StatusLine PC1500Display::statusLine() const {
    StatusLine line;
    line.decodeCommonSets(at(pc1500ram::kStatusSet0), at(pc1500ram::kStatusSet1));
    return line;
}
