#include "PC1600Memory.hpp"

#include <algorithm>

#include <cstring>

#ifdef PC1600_POWER_PROBE
#include "PC1600PowerProbe.hpp" // throw-away OFF-key trace instrumentation
#endif

bool PC1600Memory::loadBank0(const uint8_t* lower, size_t lowerSize,
                              const uint8_t* upper, size_t upperSize) {
    if (lowerSize != kBankSize || upperSize != kBankSize) return false;
    std::memcpy(m_bank0Lower.data(), lower, kBankSize);
    std::memcpy(m_bank0Upper.data(), upper, kBankSize);
    m_bank0Loaded = true;
    return true;
}

bool PC1600Memory::loadBank3Rom(const uint8_t* data, size_t size) {
    if (size != kBankSize) return false;
    std::memcpy(m_bank3Rom.data(), data, kBankSize);
    m_bank3Loaded = true;
    return true;
}

bool PC1600Memory::loadBank3bRom(const uint8_t* data, size_t size) {
    if (size != kBankSize) return false;
    std::memcpy(m_bank3bRom.data(), data, kBankSize);
    m_bank3bLoaded = true;
    return true;
}

bool PC1600Memory::loadBank6Rom(const uint8_t* data, size_t size) {
    if (size != kBankSize) return false;
    std::memcpy(m_bank6Rom.data(), data, kBankSize);
    m_bank6Loaded = true;
    return true;
}

void PC1600Memory::reset() {
    // Simple reset: internal RAM is deliberately *not* cleared here (that
    // is clearInternalRam()'s job, for ALL RESET) -- a real simple reset
    // is used to break out of a hung machine-language program without
    // losing the BASIC program or variables.
    //
    // Re-latch the PB input pins to their reset levels -- PB3's flip-flop
    // samples its (pulled-up) pin exactly here on real hardware. Keep PB5's
    // current level: it is a free-running square wave from the sub-CPU,
    // not a reset-latched line.
    m_pbIn = static_cast<uint8_t>((m_pbIn & (kPbInFreeRunning | kPbInOnKey)) | kPbInResetLevels);
    m_uart.reset();
    // Port-block reset: modulation off, SDO back to its idle level.
    m_fReg = 0;
    m_sdo = true;
    m_sdoAccum = 0;
    updateBuzzerLine();
}

void PC1600Memory::advanceBuzzer(uint32_t tstates) {
    if ((m_fReg & 0x40) == 0) {
        m_piezo.advance(tstates);
        return;
    }
    // FX = phi / (64 << F0-2); SDO toggles every half of that period.
    // Codes 5-7 aren't in the TRM table; treat them as the slowest, /1024.
    const int fx = std::min(m_fReg & 0x07, 4);
    const int64_t halfPeriod = int64_t{kPC1600TStateHz} * (int64_t{64} << fx) / 2; // in T-states * kModulatorHz
    int64_t remaining = tstates;
    while (m_sdoAccum + remaining * kModulatorHz >= halfPeriod) {
        const int64_t take = (halfPeriod - m_sdoAccum + kModulatorHz - 1) / kModulatorHz;
        m_piezo.advance(static_cast<uint32_t>(take));
        remaining -= take;
        m_sdoAccum += take * kModulatorHz - halfPeriod;
        m_sdo = !m_sdo;
        updateBuzzerLine();
    }
    m_sdoAccum += remaining * kModulatorHz;
    m_piezo.advance(static_cast<uint32_t>(remaining));
}

const uint8_t* PC1600Memory::resolveConst(uint16_t addr) const {
    if (addr < 0x4000) {
        // Page A: always bank 0, regardless of PC1600Bank::pageABank().
        return m_bank0Loaded ? &m_bank0Lower[addr] : nullptr;
    }
    if (addr < 0x8000) {
        // Page B.
        const uint16_t offset = addr - 0x4000;
        const uint8_t bank = m_bank.pageBBank();
        if (bank == 0) {
            return m_bank0Loaded ? &m_bank0Upper[offset] : nullptr;
        }
        if (bank == 3) {
            if (m_bank.hiddenBasicRomSelected()) {
                return m_bank3bLoaded ? &m_bank3bRom[offset] : nullptr;
            }
            return m_bank3Loaded ? &m_bank3Rom[offset] : nullptr;
        }
        // banks 1/2: genuinely open bus, not just unresearched -- CS24
        // alone decodes this whole window into internal ROM only, no
        // memory-slot connector pin is ever asserted here (see class
        // comment). bank 6: no documented content in this window. banks
        // 4/5 (CE-1600P): real peripheral ROM, deferred until CE-1600P
        // exists. bank 7: see Page C's bank 7 comment below (same global
        // "confirmed unused" bank number, different window).
        return nullptr;
    }
    if (addr < 0xC000) {
        // Page C.
        const uint16_t offset = addr - 0x8000;
        const uint8_t bank = m_bank.pageCBank();
        if (bank == 6) {
            return m_bank6Loaded ? &m_bank6Rom[offset] : nullptr;
        }
        // banks 0/1 -> Slot 1, banks 2/3 -> Slot 2: resolved by
        // read()/writeImpl() through m_slot1Conn/m_slot2Conn, not by a
        // pointer into a local array. bank 7: dual-source confirmed (TRM
        // §2.1 + German user manual) as genuinely unused hardware-wide --
        // permanently open bus. (banks 4/5 don't exist on
        // this page -- CE-1600P's ROM is Page B's bank 4/5, see above.)
        return nullptr;
    }
    // Page D.
    const uint16_t offset = addr - 0xC000;
    const uint8_t bank = m_bank.pageDBank();
    if (bank == 0) {
        return &m_internalRam[offset];
    }
    // bank 1: the weakest-evidenced cell in the bank-switching doc -- a
    // single low-confidence emulator-derived hypothesis (external system
    // bus routing), no Sharp-manual confirmation. Left open bus rather
    // than built on it -- see class comment.
    return nullptr;
}

bool PC1600Memory::isRom(uint16_t addr) const {
    if (addr < 0x4000) return true; // page A: system ROM, always
    if (addr < 0x8000) {
        const uint8_t bank = m_bank.pageBBank();
        return bank == 0 || bank == 3; // bank0 upper-ROM, bank3/3b normal-or-hidden ROM
    }
    if (addr < 0xC000) {
        const uint8_t bank = m_bank.pageCBank();
        return bank == 6 || bank > 3; // bank6 ROM; banks 4/5/7 unbacked (treated non-writable, harmless)
                                        // banks 0-3 (Slot 1/2 RAM): NOT ROM, writable when attached
    }
    return m_bank.pageDBank() != 0; // bank0 = internal RAM (writable); bank1 = unbacked
}

uint8_t* PC1600Memory::resolveMutable(uint16_t addr) {
    if (addr >= 0xC000) {
        if (m_bank.pageDBank() != 0) return nullptr;
        return &m_internalRam[addr - 0xC000];
    }
    // page C (8000-BFFF): resolveConst() only ever returns bank-6 ROM here,
    // which isRom() already rejected before this call; Slot 1/2 RAM writes
    // go through the connectors in writeImpl(), not this pointer path.
    return nullptr;
}

bool PC1600Memory::slot2MapTarget(uint16_t addr, bool* pvoutHigh, uint16_t* offset) const {
    switch (m_bank.slot2MapMode()) {
        case 1:
            // Mode 1: Slot 2's low 16 KB also answers at page-C bank 1
            // (which the ordinary decode would give to Slot 1). SLOT2MAP
            // only arms this -- the page-C bank register must actually
            // select bank 1 for the redirect to take effect.
            if (addr >= 0x8000 && addr < 0xC000 && m_bank.pageCBank() == 1) {
                *pvoutHigh = false;
                *offset = static_cast<uint16_t>(addr & 0x3FFF);
                return true;
            }
            return false;
        case 2:
            // Mode 2: Slot 2's low 16 KB at page-B bank 1 (4000-7FFF), its
            // high 16 KB at page-A bank 1 (0000-3FFF) -- again gated on the
            // matching page bank register selecting bank 1.
            if (addr < 0x4000 && m_bank.pageABank() == 1) {
                *pvoutHigh = true;
                *offset = static_cast<uint16_t>(addr & 0x3FFF);
                return true;
            }
            if (addr >= 0x4000 && addr < 0x8000 && m_bank.pageBBank() == 1) {
                *pvoutHigh = false;
                *offset = static_cast<uint16_t>(addr & 0x3FFF);
                return true;
            }
            return false;
        default:
            return false;
    }
}

bool PC1600Memory::slot1MapTarget(uint16_t addr, bool* pvoutHigh, uint16_t* offset) const {
    if (!m_bank.slot1MapActive()) return false;
    // SLOT1MAP=1 (TRM 0196H): Slot 1's high 16 KB half (beta) also answers
    // at page-B bank 1 (4000-7FFF), a mirror onto its normal page-C bank-1
    // home -- gated on the page-B bank register actually selecting bank 1.
    if (addr >= 0x4000 && addr < 0x8000 && m_bank.pageBBank() == 1) {
        *pvoutHigh = true; // beta = Slot 1's upper 16 KB half
        *offset = static_cast<uint16_t>(addr & 0x3FFF);
        return true;
    }
    return false;
}

uint8_t PC1600Memory::read(uint16_t addr) const {
    if (addr == kPort3DMirrorAddr) {
        return m_bank.port3DLatch();
    }
#ifdef PC1600_POWER_PROBE
    // Report which branch served the read, so a probe can tell a genuine
    // card hit from a ROM/open-bus miss (see PC1600PowerProbe's onSlotRead).
    const auto trace = [&](uint8_t v, int src) {
        pc1600probe::onSlotRead(addr, v, src, m_bank.readPort31(),
                                m_bank.slot2VerticalBank(), m_bank.readPort3C());
        return v;
    };
#else
    const auto trace = [](uint8_t v, int) { return v; };
#endif
    const SlotRemap r = resolveSlotRemap(addr);
    if (r.hit1) {
        // A Slot 1 card claims this SLOT1MAP-remapped access (or won the
        // page-B/bank-1 collision against SLOT2MAP mode 2); if the slot is
        // empty the redirect is inert and the address falls through to its
        // ordinary meaning.
        uint8_t v;
        if (m_slot1Conn.readRemapped(r.off1, r.pvoutHigh1, v)) return trace(v, 5);
    }
    if (r.hit2) {
        // A Slot 2 card claims this remapped access; if the slot is empty
        // the redirect is inert and the address falls through to its
        // ordinary meaning (ROM / Slot 1 / open bus).
        uint8_t v;
        if (m_slot2Conn.readRemapped(r.off2, r.pvoutHigh2, v)) return trace(v, 0);
    }
    if (const uint8_t* p = resolveConst(addr)) return trace(*p, 1);
    if (addr >= 0x4000 && addr < 0x8000) {
        // Page B banks 4/5: CE-1600P ROM, card-backed via the 60-pin system
        // bus -- see PC1600Memory.hpp's class comment and ce1600pBus().
        const uint8_t bank = m_bank.pageBBank();
        if (bank == 4 || bank == 5) {
            uint8_t v;
            if (m_ce1600pBus.readRom(static_cast<uint16_t>(addr - 0x4000), bank == 5, v))
                return trace(v, 6);
        }
    }
    if (isSlotWindow(addr)) { // only page C can ever reach a slot -- see isSlotWindow()
        uint8_t v;
        if (m_slot1Conn.read(addr, v)) return trace(v, 2);
        if (m_slot2Conn.read(addr, v)) return trace(v, 3);
    }
    return trace(0xFF, 4); // open bus
}

void PC1600Memory::writeImpl(uint16_t addr, uint8_t value, bool direct) {
#ifdef PC1600_POWER_PROBE
    const auto traceW = [&](int claimed) {
        pc1600probe::onSlotWrite(addr, value, claimed, m_bank.readPort31(),
                                 m_bank.slot2VerticalBank());
    };
#else
    const auto traceW = [](int) {};
#endif
    const SlotRemap r = resolveSlotRemap(addr);
    if (r.hit1 && m_slot1Conn.writeRemapped(r.off1, r.pvoutHigh1, value, direct)) {
        traceW(5);
        return;
    }
    if (r.hit2 && m_slot2Conn.writeRemapped(r.off2, r.pvoutHigh2, value, direct)) {
        traceW(0);
        return;
    }
    if (isRom(addr)) { traceW(1); return; } // ROM / unbacked: ignored
    if (uint8_t* p = resolveMutable(addr)) { *p = value; traceW(1); return; }
    if (isSlotWindow(addr)) {
        if (m_slot1Conn.write(addr, value, direct)) { traceW(2); return; }
        if (m_slot2Conn.write(addr, value, direct)) { traceW(3); return; }
    }
    traceW(4); // open bus / unattached slot: nothing to write
}

uint8_t PC1600Memory::readIO(uint8_t port) {
#ifdef PC1600_POWER_PROBE
    const uint8_t v = readIOImpl(port);
    pc1600probe::onIoRead(port, v);
    return v;
}
uint8_t PC1600Memory::readIOImpl(uint8_t port) {
#endif
    if (port >= 0x50 && port <= 0x5B) return m_display.readIO(port);
    // CE-1600P/CE-1600F (60-pin system bus): ports 0x70-0x8F -- 0x70-0x7F
    // (CE1600FCard: command/sector/motor/data registers) and 0x80-0x8F
    // (CE1600PCard: 0x81 pen-at-home status today). Open bus (0xFF) when
    // unattached or when no card on the bus claims a given port, same
    // convention as the rest of this function.
    if (port >= 0x70 && port <= 0x8F) {
        uint8_t v;
        return m_ce1600pBus.readIO(port, v) ? v : 0xFF;
    }
    switch (port) {
        case 0x31: return m_bank.readPort31();
        case 0x32: { uint8_t v = m_intCause; m_intCause = 0; updateIntLine(); return v; } // read-clears, see latchTimer64InterruptCause()'s own comment
        // 33H (IOR P) = the sub-CPU's answer register; 21H's write side is
        // the matching command port. See PC1600SubCpu's class comment.
        case 0x33: return m_subCpu.readAnswer();
        // 35H (IOR ZMSK) = read back the SC-7852 interrupt mask written via
        // this port's write side. The timer ISR reads it at PC1600-P1-B3-new.bin
        // 4102H/4112H to decide which pending causes are unmasked.
        case 0x35: return m_intMask;
        case 0x17: return m_fReg;
        case 0x18: return m_opc;
        // MSK read: the mask in bits 0-3, and the live CL1/SD1/PB7/IRQ inputs
        // in bits 7-4 (PC-1500 TRM p.71). Only PB7 (the ON key, bit 5) has a
        // source here; the others read 0.
        case 0x1A: return static_cast<uint8_t>((m_msk & 0x0F) | ((m_pbIn & kPbInOnKey) ? 0x20 : 0x00));
        case 0x1B: return m_if;
        case 0x1C: return m_dda;
        case 0x1D: return m_ddb;
        case 0x1E: return m_opa;
        // Per PC-1600-IO-Ports.md §2.4: a PB bit configured as an output
        // (DDB.i = 1) reads back the OPB latch; one configured as an input
        // reads the live pin level. Merging the two is what keeps PB5's
        // 64Hz square wave alive across the ROM's own read-modify-write of
        // this port -- see setTimer64Bit()'s comment for the trace evidence.
        case 0x1F: return static_cast<uint8_t>((m_opb & m_ddb) |
                                                (m_pbIn & static_cast<uint8_t>(~m_ddb)));
        case 0x37: {
            // PB6 is a key strobe for CTRL/KBII/BS. Per the PC-1600 service
            // manual's PB6 description it drives the strobe low only while
            // configured as an *output*; releasing the strobe means "set
            // OPB.6 high, then switch PB6 to input", where an internal
            // pull-up holds the line high. So the strobe is asserted only
            // when PB6 is an output (DDB.6 = 1) AND OPB.6 = 0 -- checking
            // OPB.6 alone would keep KBII (KIN1) bleeding onto the KS6 pass
            // if the ROM releases by flipping to input without the courtesy
            // high write, colliding with MODE (also KS6/KIN1).
            bool pb6Strobing = (m_ddb & 0x40) != 0 && (m_opb & 0x40) == 0;
            return m_keyboard.scan(m_opa, pb6Strobing);
        }
        // TC8576F UART register file (20H-27H, A1:A0 = port & 3): 20H RxD,
        // 21H PIN, 22H SSR, 23H PSR. The sub-CPU answer at 33H is a
        // separate register (IOR P, the LR38041 return buffer) and stays
        // routed to m_subCpu below -- it is not a UART register.
        case 0x20: case 0x21: case 0x22: case 0x23:
        case 0x24: case 0x25: case 0x26: case 0x27:
            return m_uart.readRegister(port & 0x03);
        default:   return 0xFF; // open bus -- see class comment for scope
    }
}

void PC1600Memory::writeIO(uint8_t port, uint8_t value) {
#ifdef PC1600_POWER_PROBE
    pc1600probe::onIoWrite(port, value);
#endif
    if (port >= 0x50 && port <= 0x5B) { m_display.writeIO(port, value); return; }
    // CE-1600P/CE-1600F (60-pin system bus): ports 0x70-0x8F -- 0x70-0x7F
    // (CE1600FCard registers) and 0x80-0x8F (0x81 FD reset, 0x82 Z-motor,
    // 0x83 X/Y-motor; see CE1600PCard/CE1600FCard). No-op when unattached.
    if (port >= 0x70 && port <= 0x8F) { m_ce1600pBus.writeIO(port, value); return; }
    // Slot-2 I/O range (28-2FH, PC-1600 TRM §9): a module whose bank latch
    // triggers on a port write (CE-1601M: OUT (28H) sampling the data bus)
    // sees it here -- the card now owns that decode. The mainboard's own
    // Port 28H latch below is kept only as a readback register
    // (slot2VerticalBank()); nothing outside the bank tests reads it.
    if (port >= 0x28 && port <= 0x2F) m_slot2Conn.ioWrite(port, value);
    switch (port) {
        case 0x28: m_bank.writePort28(value); return;
        // TC8576F UART register file (20H-27H). 21H's write side (PVOUT,
        // parallel data out) is where the sub-CPU command byte physically
        // leaves -- TC8576F::writeRegister forwards it to
        // PC1600SubCpu::command(). 22H = parameter register, 23H = command
        // register + parameter-address pointer, 20H = serial TxD.
        case 0x20: case 0x21: case 0x22: case 0x23:
        case 0x24: case 0x25: case 0x26: case 0x27:
            m_uart.writeRegister(port & 0x03, value); return;
        case 0x31: m_bank.writePort31(value); return;
        // 3CH = SLOT1MAP/SLOT2MAP gate-array control. The firmware keeps a
        // RAM shadow at F08DH (ordinary internal RAM -- no mirror needed
        // here, unlike Port 3DH's F07DH); slot2MapTarget() reads the
        // authoritative b5:b4 from this latch.
        case 0x3C: m_bank.writePort3C(value); return;
        case 0x3D: m_bank.writePort3D(value); return;
        case 0x35: m_intMask = value; updateIntLine(); return;
        case 0x39: m_im2VectorLow = value; if (m_cpu) m_cpu->setIM2VectorByte(value); return;
        case 0x38: if (m_arbiter) m_arbiter->requestSwitchFromSC7852(); return;
        case 0x18:
            // Buzzer: bit 6 = enable (BEEP ON/OFF), bit 7 = the square
            // wave the BEEP loop toggles. See m_opc.
            m_opc = value;
            updateBuzzerLine();
            return;
        // 14H: divider reset -- restart the modulation clocks' phase.
        case 0x14: m_sdoAccum = 0; return;
        // 17H: F register -- SDO modulation (see m_fReg).
        case 0x17:
            m_fReg = static_cast<uint8_t>(value & 0x7F);
            if ((m_fReg & 0x40) == 0) m_sdo = true; // normal mode: SDO = SXO = idle mark
            updateBuzzerLine();
            return;
        case 0x1A: m_msk = static_cast<uint8_t>(value & 0x0F); return;
        case 0x1B: m_if = value; return;
        case 0x1C: m_dda = value; return;
        case 0x1D: m_ddb = value; return;
        case 0x1E: m_opa = value; return;
        case 0x1F: m_opb = value; return;
        case 0x37: m_display.setClockEnabled((value & 0x10) != 0); return;
        // 36H is part of the SC-7852's internal LSI control-register block
        // (30H-3FH; read = IOR ADRS, write = IOW CL1), per the TRM's own
        // Z-80/LH-5803 I/O-address table. That table carries a standing
        // caution for the whole block -- "when writing to an I/O space
        // between 30H and 3DH, if the setting is incorrect, the PC-1600
        // does not operate properly" -- so treat any *new* handler added in
        // this range as behaviour-affecting until proven otherwise.
        // Deliberately a no-op rather than falling through to the open-bus
        // default: the timer ISR writes EFH here every time it runs, at
        // PC1600-P1-B3-new.bin 4197H, immediately after the `IN A,(32H)` cause
        // read at 40FFH -- i.e. an interrupt acknowledge/re-arm -- and
        // interrupts keep arriving with it ignored, so nothing in this
        // core needs the register's contents yet. Recorded here so it
        // stops reading as an unidentified port.
        case 0x36: return;
        default:   return; // open bus -- see class comment for scope
    }
}
