#include "LH5803Memory.hpp"

void LH5803Memory::reset() {
    // RAM powers up as 0x00 -- CMOS RAM without supply comes back (mostly)
    // zero, the convention used across the PC-1500/1500A/1600 family.
    m_internalRam.fill(0x00);
}

uint8_t LH5803Memory::readME0(uint16_t addr) {
    if (addr < kRamBase) return 0xFF;                    // Slot 1/2 module window, open bus
    if (addr < kRamBase + kRamSize) return m_internalRam[addr - kRamBase];
    if (addr < kRomBase) return 0xFF;                     // CE-158/150 ROM window, open bus
    return m_rom.read(addr);
}

void LH5803Memory::writeME0(uint16_t addr, uint8_t value) {
    if (addr < kRamBase) return;                          // open bus, ignored
    if (addr < kRamBase + kRamSize) { m_internalRam[addr - kRamBase] = value; return; }
    // CE-158/150 window and ROM: writes ignored.
}

uint8_t LH5803Memory::peek(uint16_t addr) const {
    if (addr < kRamBase) return 0xFF;
    if (addr < kRamBase + kRamSize) return m_internalRam[addr - kRamBase];
    if (addr < kRomBase) return 0xFF;
    return m_rom.read(addr);
}

void LH5803Memory::poke(uint16_t addr, uint8_t value) {
    if (addr < kRamBase) return;
    if (addr < kRamBase + kRamSize) { m_internalRam[addr - kRamBase] = value; return; }
}
