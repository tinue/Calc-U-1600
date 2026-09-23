#include "LH5803SharedMemory.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

#include "../../PC1600/PC1600BusArbiter.hpp"
#include "../../PC1600/PC1600Memory.hpp"

bool LH5803SharedMemory::loadROM(const uint8_t* data, size_t size) {
    if (size != kRomSize) return false;
    std::memcpy(m_rom.data(), data, kRomSize);
    m_romLoaded = true;
    return true;
}

bool LH5803SharedMemory::loadROMFile(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> buf(kRomSize + 1);
    size_t n = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (n != kRomSize) return false;
    return loadROM(buf.data(), kRomSize);
}

uint8_t LH5803SharedMemory::readME0(uint16_t addr) {
    if (addr < 0x8000) return m_shared.read(uint16_t(addr + 0x8000));
    if (addr < kRomBase) {
        // 8000-BFFF peripheral-ROM window, selected by the LH5803's own PV
        // (the ROM sets it from CALLH's PARBAN): CE-150 ROM at PVOUT=0
        // (upper 8K), CE-158 ROM at PVOUT=1 (lower 8K, PU-banked). Each
        // card gates its window on the PV/PU it is shown.
        uint8_t v;
        const PinState p = peripheralPins(addr, /*forWrite=*/false, /*me1=*/false);
        if (m_ce158 && m_ce158->respondsToRead(p, v)) return v;
        if (m_ce150 && m_ce150->respondsToRead(p, v)) return v;
        return 0xFF; // open bus
    }
    return m_romLoaded ? m_rom[addr - kRomBase] : 0xFF;
}

void LH5803SharedMemory::writeME0(uint16_t addr, uint8_t value) {
    if (addr < 0x8000) { m_shared.write(uint16_t(addr + 0x8000), value); return; }
    // CE-158/150 ROM window and the LH5803-private ROM: writes ignored.
}

bool LH5803SharedMemory::isUartShadow(uint16_t addr, uint8_t* reg, bool* isSubCpuAnswer) {
    const uint16_t off = addr & 0x00FF; // 0xA0xx shadow and the bare form share the low byte
    if ((addr & 0xFF00) != 0x0000 && (addr & 0xFF00) != 0xA000) return false;
    if (off >= 0x20 && off <= 0x27) { *reg = uint8_t(off & 0x03); *isSubCpuAnswer = false; return true; }
    if (off == 0x33)                { *reg = 0; *isSubCpuAnswer = true; return true; }
    return false;
}

uint8_t LH5803SharedMemory::readME1(uint16_t addr) {
    // The CE-150's LH5810 chip-select covers the whole ME1 0xB000-0xB00F
    // block (only 0xB008-0xB00F are registers the card decodes; the rest is
    // claimed-but-inert, exactly as on real hardware). This branch is
    // TERMINAL: without it, an ME1 read of 0xB000-0xB007 would
    // fall through to readME0() and be mis-served as CE-150 *ROM* bytes
    // (0xA000-0xBFFF is the ROM window in ME0), which breaks the plotter
    // ROM's inter-step LH5810 polls -- LPRINT then prints one character and
    // aborts.
    if (m_ce150 && (addr & 0xFFF0) == 0xB000) {
        uint8_t v = 0xFF;
        m_ce150->respondsToRead(peripheralPins(addr, /*forWrite=*/false, /*me1=*/true), v); // v left 0xFF for the non-register part
        return v;
    }
    // CE-158 register blocks: terminal for the same reason -- falling
    // through would serve ROM bytes (0xD000+ >= kRomBase) as I/O.
    if (m_ce158 && isCe158Io(addr)) {
        uint8_t v = 0xFF;
        m_ce158->respondsToRead(peripheralPins(addr, /*forWrite=*/false, /*me1=*/true), v);
        return v;
    }
    uint8_t reg; bool answer;
    if (isUartShadow(addr, &reg, &answer)) {
        return answer ? m_shared.subCpu().readAnswer()
                      : m_shared.uart().readRegister(reg);
    }
    // LH5803 on-chip LH5811-compat PIO, ME1 0xF000-0xF00F. Unconditional
    // (CPU-internal, present with or without a CE-150). Without this, an
    // ME1 read here falls through to readME0() and 0xF00B >= kRomBase
    // returns a PC1600-LH5803-C000-FFFF-new.bin byte (0x27) -- bit 1 set -- so the CE-150 plot
    // loop's `BII #(0xF00B),0x02` pacing poll takes the wrong arm and
    // LPRINT/TEST draw one glyph then unwind. IF (0xB) is software-only: no
    // clear-on-read (firmware clears bit 1 with `ani #(0xF00B),0xFD`), and
    // no PC-1600 TP/RTC edge feeds it yet. Cf. PC1500Memory's `case 0xB`.
    if ((addr & 0xFFF0) == 0xF000) {
        return m_ioRegs[addr & 0x0F];
    }
    return readME0(addr); // default aliasing -- no other read-side trigger
}

void LH5803SharedMemory::writeME1(uint16_t addr, uint8_t value) {
    if (addr == kHandoffTriggerAddr) {
        if (m_arbiter) m_arbiter->requestSwitchFromLH5803();
        return;
    }
    if (m_ce150 && (addr & 0xFFF0) == 0xB000) {
        // Terminal, same reasoning as readME1(): a write to the LH5810
        // window must never fall through to writeME0().
        m_ce150->respondsToWrite(peripheralPins(addr, /*forWrite=*/true, /*me1=*/true), value);
        return;
    }
    if (m_ce158 && isCe158Io(addr)) {
        m_ce158->respondsToWrite(peripheralPins(addr, /*forWrite=*/true, /*me1=*/true), value);
        return;
    }
    uint8_t reg; bool answer;
    if (isUartShadow(addr, &reg, &answer)) {
        if (!answer) m_shared.uart().writeRegister(reg, value); // 33H is read-only
        return;
    }
    // Internal LH5811-compat PIO, ME1 0xF000-0xF00F -- straight-through
    // latch (IF at 0xB included). See readME1() for the rationale.
    if ((addr & 0xFFF0) == 0xF000) {
        m_ioRegs[addr & 0x0F] = value;
        return;
    }
    writeME0(addr, value); // default aliasing for every other ME1 address
}
