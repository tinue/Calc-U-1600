#include "LH5803SharedMemory.hpp"

#include "../../Display/Pc1500DisplayRam.hpp"
#include "../../PC1600/PC1600BusArbiter.hpp"
#include "../../PC1600/PC1600Memory.hpp"

uint8_t LH5803SharedMemory::readME0(uint16_t addr) {
    return onBus(readME0Driven(addr));
}

uint8_t LH5803SharedMemory::readME0Driven(uint16_t addr) {
    if (addr < 0x8000) return m_shared.read(uint16_t(lha90(addr) + 0x8000));
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
    m_dataBus = value;
    if (addr < 0x8000) {
        addr = lha90(addr);
        m_shared.write(uint16_t(addr + 0x8000), value);
        mirrorPc1500Display(addr);
        return;
    }
    // CE-158/150 ROM window and the LH5803-private ROM: writes ignored.
}

void LH5803SharedMemory::mirrorPc1500Display(uint16_t addr) {
    PC1600Display& display = m_shared.display();
    if (addr == pc1500ram::kStatusSet0 || addr == pc1500ram::kStatusSet1) {
        display.mirrorPc1500StatusSet(addr == pc1500ram::kStatusSet0 ? 0 : 1, m_shared.read(uint16_t(addr + 0x8000)));
        return;
    }
    if (!pc1500ram::isColumnByte(addr)) return;
    // The whole column, from both bytes of its pair as they are in RAM now.
    const uint16_t pair = uint16_t(addr & ~1u);
    const uint8_t first = m_shared.read(uint16_t(pair + 0x8000));
    const uint8_t second = m_shared.read(uint16_t(pair + 1 + 0x8000));
    for (int block = 0; block < 2; ++block)
        display.mirrorPc1500Column(pc1500ram::columnOf(pair, block), pc1500ram::columnDots(first, second, block));
}

uint8_t LH5803SharedMemory::readInputPort() {
    return m_shared.readIO(0x37);
}

bool LH5803SharedMemory::isUartShadow(uint16_t addr, uint8_t* reg, bool* isSubCpuAnswer) {
    const uint16_t off = addr & 0x00FF; // 0xA0xx shadow and the bare form share the low byte
    if ((addr & 0xFF00) != 0x0000 && (addr & 0xFF00) != 0xA000) return false;
    if (off >= 0x20 && off <= 0x27) { *reg = uint8_t(off & 0x03); *isSubCpuAnswer = false; return true; }
    if (off == 0x33)                { *reg = 0; *isSubCpuAnswer = true; return true; }
    return false;
}

bool LH5803SharedMemory::cardRead(uint16_t addr, bool me1, uint8_t* value) const {
    const auto& bus = m_shared.systemBus();
    return !bus.empty() && bus.read(busPins(addr, /*forWrite=*/false, me1), *value);
}

bool LH5803SharedMemory::cardWrite(uint16_t addr, bool me1, uint8_t value) {
    auto& bus = m_shared.systemBus();
    return !bus.empty() && bus.write(busPins(addr, /*forWrite=*/true, me1), value);
}

uint8_t LH5803SharedMemory::debugPeek(uint16_t addr, bool me1, bool* readable) const {
    *readable = true;
    if (me1) {
        uint8_t reg; bool answer;
        // isLcdPort(): an HD61102 data read advances its address counter;
        // isControlPort(): a 32H read clears the interrupt cause.
        if (isUartShadow(addr, &reg, &answer) || isLcdPort(addr) || isControlPort(addr) ||
            (addr >= 0x8000 && addr < kRomBase)) {
            *readable = false;
            return 0xFF;
        }
        // Ports 10H-1FH have no read side effects.
        if ((addr & 0xFFF0) == 0xF000) return m_shared.readIO(uint8_t(0x10 | (addr & 0x0F)));
        if (addr >= 0x8000) {
            // Same order as readME1(): a card gets the rest of the ME1
            // upper half; one whose register a read would disturb stays
            // unread.
            const SystemBusPins p = busPins(addr, /*forWrite=*/false, /*me1=*/true);
            const auto& bus = m_shared.systemBus();
            if (bus.readHasSideEffects(p)) {
                *readable = false;
                return 0xFF;
            }
            uint8_t v;
            if (bus.read(p, v)) return v;
        }
        // Nothing answers: the bus floats (readME1()), no value to show.
        *readable = false;
        return 0xFF;
    }
    // ME0 (LHA90 included, as readME0() applies it)
    uint16_t z80 = 0;
    if (toZ80Address(lha90(addr), &z80)) return m_shared.peek(z80);
    uint8_t v;
    if (addr < kRomBase) return cardRead(addr, /*me1=*/false, &v) ? v : 0xFF;
    return m_rom.read(addr);
}

std::string LH5803SharedMemory::debugBusCardAt(uint16_t addr, bool me1, bool pu, bool pv) const {
    const SystemBusPins pins = PC1600BusDrive::lh5803Pins(addr, /*forWrite=*/false, me1, pu, pv);
    for (const SystemBusCard* card : m_shared.systemBus().chain()) {
        uint8_t v;
        // A register a read would disturb is still the card's: it says so
        // without being read.
        if (card->readHasSideEffects(pins) || card->respondsToRead(pins, v)) return card->moduleName();
    }
    return {};
}

uint8_t LH5803SharedMemory::readME1(uint16_t addr) {
    uint8_t v;
    if (readME1Driven(addr, &v)) return onBus(v);
    // Nothing drives the data bus: ME1 is the SC7852's IORQ, so no RAM or
    // ROM answers, and the bus still holds the cycle before's byte -- on a
    // real PC-1600 XPEEK# reads 37 (25H, the second byte of its LDA #(U),
    // FD 25H) at every address tried, C000H included (2026-10-10,
    // docs/background/Decisions.md).
    return m_dataBus;
}

bool LH5803SharedMemory::readME1Driven(uint16_t addr, uint8_t* out) {
    uint8_t& v = *out;
    uint8_t reg; bool answer;
    if (isUartShadow(addr, &reg, &answer)) {
        v = answer ? m_shared.subCpu().readAnswer() : m_shared.uart().readRegister(reg);
        return true;
    }
    // SC7852 control-port block 30H-3FH at ME1 A030-A03F, e.g. the bank
    // register save `LDA #(P_BANK)` at rom1500 DC85/DC9A, and the LCD ports
    // at A040-A05F / 8040-805F (the ROM's LCD busy polls). See writeME1().
    if (isControlPort(addr) || isLcdPort(addr)) {
        v = m_shared.readIO(static_cast<uint8_t>(addr));
        return true;
    }
    // ME1 0xF000-0xF00F: the SC7852's LH5810-compatible block, Z-80 ports
    // 10H-1FH (see the class comment). Before the card decode: falling
    // through to readME0() would serve LH5803 ROM bytes, and the CE-150
    // plot loop's `BII #(0xF00B),0x02` pacing poll would take the wrong arm.
    if ((addr & 0xFFF0) == 0xF000) {
        v = m_shared.readIO(uint8_t(0x10 | (addr & 0x0F)));
        return true;
    }
    // 8000-FFFF: offered to the cards as ME1 (the CE-150's LH5810 at
    // B008-B00F, the CE-158's register blocks at D000-D3FF / DE00-DFFF).
    // Never the RAM, the peripheral ROMs or the LH5803 ROM: an ME1 cycle is
    // an I/O cycle (IORQ).
    return addr >= 0x8000 && cardRead(addr, /*me1=*/true, &v);
}

void LH5803SharedMemory::writeME1(uint16_t addr, uint8_t value) {
    m_dataBus = value;
    if (addr == kHandoffTriggerAddr) {
        if (m_arbiter) m_arbiter->requestSwitchFromLH5803();
        return;
    }
    uint8_t reg; bool answer;
    if (isUartShadow(addr, &reg, &answer)) {
        if (!answer) m_shared.uart().writeRegister(reg, value); // 33H is read-only
        return;
    }
    // SC7852 control-port block 30H-3FH at ME1 A030-A03F (A038 is the
    // handoff, above), and the LCD ports at A040-A05F / 8040-805F. rom1500 writes P_MOD/P_BANK/P_LHMSK2/P_CL1 here; the
    // one that matters today is P_MAPPRG's `STA #(P_BANK)` (E652 -> DC94),
    // which maps the BASIC program bank into page C for MODE 1 PEEK/XPEEK.
    // Dropping it left page C on whatever the Z-80 had set (bank 0).
    if (isControlPort(addr) || isLcdPort(addr)) {
        m_shared.writeIO(static_cast<uint8_t>(addr), value);
        return;
    }
    // ME1 0xF000-0xF00F: Z-80 ports 10H-1FH, see readME1().
    if ((addr & 0xFFF0) == 0xF000) {
        m_shared.writeIO(uint8_t(0x10 | (addr & 0x0F)), value);
        return;
    }
    // 8000-FFFF: an ME1 I/O cycle for the cards, see readME1(). Anything
    // unclaimed goes nowhere: no RAM answers an ME1 cycle.
    if (addr >= 0x8000) cardWrite(addr, /*me1=*/true, value);
}
