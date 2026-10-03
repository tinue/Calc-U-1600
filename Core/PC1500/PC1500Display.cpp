#include "PC1500Display.hpp"

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

    // 156 columns split into two 78-column blocks; each block's 78 columns
    // split into two 39-column halves (base 0x7600 vs 0x7700); each of the
    // 39 columns in a half reads one byte pair, one nibble of which belongs
    // to this block. See this file's header comment for provenance.
    int block = col / 78;
    int withinBlock = col % 78;
    int half = withinBlock < 39 ? 0 : 1;
    int local = withinBlock % 39;

    uint16_t base = half == 0 ? 0x7600 : 0x7700;
    uint16_t addr = uint16_t(base + local * 2);
    uint8_t byte0 = at(addr);
    uint8_t byte1 = at(uint16_t(addr + 1));
    uint8_t nibble0 = block == 0 ? uint8_t(byte0 & 0x0F) : uint8_t(byte0 >> 4);
    uint8_t nibble1 = block == 0 ? uint8_t(byte1 & 0x0F) : uint8_t(byte1 >> 4);
    uint8_t data8 = uint8_t(nibble0 | (nibble1 << 4));

    return ((data8 >> row) & 1) != 0;
}

StatusLine PC1500Display::statusLine() const {
    StatusLine line;
    line.decodeCommonSets(at(0x764E), at(0x764F));
    return line;
}
