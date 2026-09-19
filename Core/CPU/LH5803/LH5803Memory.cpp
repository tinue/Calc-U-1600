#include "LH5803Memory.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

bool LH5803Memory::loadROM(const uint8_t* data, size_t size) {
    if (size != kRomSize) return false;
    std::memcpy(m_rom.data(), data, kRomSize);
    m_romLoaded = true;
    return true;
}

bool LH5803Memory::loadROMFile(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> buf(kRomSize + 1);
    size_t n = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (n != kRomSize) return false;
    return loadROM(buf.data(), kRomSize);
}

void LH5803Memory::reset() {
    // RAM powers up as 0x00 -- CMOS RAM without supply comes back (mostly)
    // zero, the convention used across the PC-1500/1500A/1600 family.
    m_internalRam.fill(0x00);
}

uint8_t LH5803Memory::readME0(uint16_t addr) {
    if (addr < kRamBase) return 0xFF;                    // Slot 1/2 module window, open bus
    if (addr < kRamBase + kRamSize) return m_internalRam[addr - kRamBase];
    if (addr < kRomBase) return 0xFF;                     // CE-158/150 ROM window, open bus
    return m_romLoaded ? m_rom[addr - kRomBase] : 0xFF;
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
    return m_romLoaded ? m_rom[addr - kRomBase] : 0xFF;
}

void LH5803Memory::poke(uint16_t addr, uint8_t value) {
    if (addr < kRamBase) return;
    if (addr < kRamBase + kRamSize) { m_internalRam[addr - kRamBase] = value; return; }
}
