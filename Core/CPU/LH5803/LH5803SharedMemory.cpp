#include "LH5803SharedMemory.hpp"

#include "../../PC1600/PC1600BusArbiter.hpp"
#include "../../PC1600/PC1600Memory.hpp"

uint8_t LH5803SharedMemory::readME0(uint16_t addr) {
    if (addr < 0x8000) return m_shared.read(uint16_t(addr + 0x8000));
    if (addr < kRomBase) {
        // 8000-BFFF peripheral-ROM window, selected by the LH5803's own PV
        // (the ROM sets it from CALLH's PARBAN): CE-150 ROM at PVOUT=0
        // (upper 8K), CE-158 ROM at PVOUT=1 (lower 8K, PU-banked). Each
        // card gates its window on the PV/PU it is shown.
        uint8_t v;
        return cardRead(addr, /*me1=*/false, &v) ? v : 0xFF;
    }
    return m_rom.read(addr);
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

bool LH5803SharedMemory::cardRead(uint16_t addr, bool me1, uint8_t* value) const {
    const auto& bus = m_shared.lh5803PeripheralBus();
    return !bus.empty() && bus.read(peripheralPins(addr, /*forWrite=*/false, me1), *value);
}

bool LH5803SharedMemory::cardWrite(uint16_t addr, bool me1, uint8_t value) {
    auto& bus = m_shared.lh5803PeripheralBus();
    return !bus.empty() && bus.write(peripheralPins(addr, /*forWrite=*/true, me1), value);
}

uint8_t LH5803SharedMemory::debugPeek(uint16_t addr, bool me1, bool* readable) const {
    *readable = true;
    if (me1) {
        uint8_t reg; bool answer;
        if (isUartShadow(addr, &reg, &answer) || (addr >= 0x8000 && addr < kRomBase)) {
            *readable = false;
            return 0xFF;
        }
        if ((addr & 0xFFF0) == 0xF000) return m_ioRegs[addr & 0x0F];
        if (addr >= 0x8000) {
            // Same order as readME1(): a card gets the rest of the ME1
            // upper half; one whose register a read would disturb stays
            // unread.
            const PinState p = peripheralPins(addr, /*forWrite=*/false, /*me1=*/true);
            const auto& bus = m_shared.lh5803PeripheralBus();
            if (bus.readHasSideEffects(p)) {
                *readable = false;
                return 0xFF;
            }
            uint8_t v;
            if (bus.read(p, v)) return v;
        }
    }
    // ME0, and the ME1 addresses readME1() aliases onto it
    uint16_t z80 = 0;
    if (toZ80Address(addr, &z80)) return m_shared.peek(z80);
    uint8_t v;
    if (addr < kRomBase) return cardRead(addr, /*me1=*/false, &v) ? v : 0xFF;
    return m_rom.read(addr);
}

uint8_t LH5803SharedMemory::readME1(uint16_t addr) {
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
    // 8000-FFFF: offered to the cards as ME1, terminal when one claims it
    // (the CE-150's LH5810 at B008-B00F, the CE-158's register blocks at
    // D000-D3FF / DE00-DFFF). A claimed read in C000-FFFF must not fall
    // through, or the LH5803 ROM bytes would be served as I/O.
    if (addr >= 0x8000) {
        uint8_t v;
        if (cardRead(addr, /*me1=*/true, &v)) return v;
    }
    // 8000-BFFF unclaimed: ME1 reaches the bus as an I/O cycle (IORQ), so
    // it never selects a peripheral ROM -- open bus. Aliasing this to
    // readME0() served CE-150/CE-158 ROM bytes as I/O (the CE-150 LPRINT
    // one-character bug came from exactly that at B000-B007).
    if (addr >= 0x8000 && addr < kRomBase) return 0xFF;
    return readME0(addr); // default aliasing -- no other read-side trigger
}

void LH5803SharedMemory::writeME1(uint16_t addr, uint8_t value) {
    if (addr == kHandoffTriggerAddr) {
        if (m_arbiter) m_arbiter->requestSwitchFromLH5803();
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
    // 8000-FFFF: an ME1 I/O cycle for the cards, see readME1(). Unclaimed
    // 8000-BFFF goes nowhere; the rest keeps the default aliasing (which
    // ignores writes at 8000+ anyway).
    if (addr >= 0x8000 && cardWrite(addr, /*me1=*/true, value)) return;
    if (addr >= 0x8000 && addr < kRomBase) return;
    writeME0(addr, value); // default aliasing for every other ME1 address
}
