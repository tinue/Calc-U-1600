#include <algorithm>
#include "PC1500Memory.hpp"

#include <cstdio>
#include <vector>

#include "../Connector/ExpansionConnector.hpp"
#include "../Connector/SystemBus.hpp"

namespace {
// The CE-150's LH5810 register block lives in ME1 at 0xB008-0xB00F (see
// Core/Connector/Ce150Card.hpp). On real hardware that range also aliases
// the internal LH5811 -- isIoChipAddress() matches any ME1 address with
// bits 12-13 set -- and the firmware only avoids the clash by always
// addressing its own chip at 0xF00x. A 60-pin card that claims this window
// decodes it itself, so readME1()/writeME1() give the SystemBus first
// refusal here; with no card on the chain SystemBus::{read,write}ME1
// returns false and the internal-chip path below runs unchanged.
constexpr uint16_t kCe150IoBase = 0xB008;
constexpr uint16_t kCe150IoEnd  = 0xB00F;
} // namespace

PC1500Memory::PC1500Memory(PC1500Variant variant)
    : m_variant(variant),
      m_userRamSize(variant == PC1500Variant::PC1500A ? kUserRamSizeA : kUserRamSizePlain),
      m_systemRamAddrMask(variant == PC1500Variant::PC1500A ? 0x7FF : 0x3FF) {
    m_rom.fill(0xFF);  // open until a ROM is loaded
    // Power-up: user CMOS RAM that has lost its supply comes back (mostly)
    // zero on real hardware, not 0xFF -- modelled as all 0x00. The 1.5K at
    // &7600-&7BFF (display RAM + the 1K system RAM) instead reads 0xFF after a
    // power loss on a real PC-1500 (measured), so that window is filled 0xFF.
    clearRam();
}

void PC1500Memory::clearRam() {
    m_userRam.fill(0x00);
    m_displayRam.fill(0xFF);
    m_systemRam.fill(0x00);
    // &7800-&7BFF; a PC-1500A's upper 1K (&7C00-&7FFF) is unmeasured, left 0.
    std::fill_n(m_systemRam.begin(), 0x400, uint8_t{0xFF});
}

bool PC1500Memory::loadROM(const uint8_t* data, size_t size) {
    if (size != kRomSize) return false;
    for (size_t i = 0; i < kRomSize; i++) m_rom[i] = data[i];
    return true;
}

bool PC1500Memory::loadROMFile(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> buf(kRomSize + 1);
    size_t n = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (n != kRomSize) return false;
    return loadROM(buf.data(), kRomSize);
}

void PC1500Memory::reset() {
    // A real RESET leaves RAM alone -- the BASIC program, variables and
    // system area survive it, and the ROM's own start-up code decides what
    // to keep. Clearing RAM is clearRam()'s job (power-up, ALL RESET).
    m_dda = m_opa = m_ddb = m_opb = 0;
    m_opc = 0;
    m_piezo.setLevel(false);
    m_if = 0;
    m_rtc = Upd1990ac{}; // fresh chip state -- TP un-configured until the ROM issues a rate-select, same as real power-on
    m_ioScratchRegs.fill(0);
    // Keyboard/ON-key state deliberately untouched -- a CPU reset doesn't
    // release physically-held keys.
}

bool PC1500Memory::inhibitAsserted() const {
    return (m_expansionConnector && m_expansionConnector->inhibitAsserted()) ||
           (m_systemBus && m_systemBus->inhibitAsserted());
}

const uint8_t* PC1500Memory::resolve(uint16_t addr, bool forWrite) const {
    if (addr >= kRomBase) {
        // ROM never accepts writes, regardless of INHIBIT -- checked first
        // so a write doesn't pay for the INHIBIT query below.
        if (forWrite) return nullptr;
        // A card on either connector can pull INHIBIT low to suppress the
        // system ROM and substitute its own content there (Expansion-
        // Connectors.md §2.1/§2.2's INHIBIT pin) -- readME0/peek fall
        // through to the connectors below when this returns nullptr, so
        // this is the ROM's whole story here; the connector's own response
        // (if any) is what actually supplies the byte.
        if (inhibitAsserted()) return nullptr;
        return &m_rom[addr - kRomBase];
    }
    if (addr >= kUserRamBase && addr < kUserRamBase + m_userRamSize) {
        return &m_userRam[addr - kUserRamBase];
    }
    if (addr >= kSystemRamBase && addr < kSystemRamBase + kSystemRamWindowSize) {
        // Plain PC-1500: the TC5514 pair only decodes A0-A9 (10 lines)
        // across this 11-line, 2KB window, so &7C00-&7FFF aliases
        // &7800-&7BFF at a fixed &400 offset (PC-1500-Address-Decoding.md
        // §2.3) -- masking off bit 10 of the window-relative offset
        // reproduces exactly that. The PC-1500A doesn't alias (mask covers
        // the full window), so one masked expression serves both models.
        return &m_systemRam[(addr - kSystemRamBase) & m_systemRamAddrMask];
    }
    if (addr >= kDisplayRamBase && addr < kDisplayRamBase + kDisplayRamWindowSize) {
        return &m_displayRam[(addr - kDisplayRamBase) % kDisplayRamSize];
    }
    return nullptr; // open bus: 0x0000-0x3FFF, 0x5800-0x6FFF, 0x8000-0xBFFF
                     // (plus 0x4800-0x57FF on the plain PC-1500)
}

uint8_t* PC1500Memory::resolve(uint16_t addr, bool forWrite) {
    return const_cast<uint8_t*>(static_cast<const PC1500Memory*>(this)->resolve(addr, forWrite));
}

uint8_t PC1500Memory::readOpenBus(uint16_t addr) const {
    // Called once resolve() has already returned nullptr: either a
    // module-slot open-bus range (Y0, the variant's open S-range, Y2) or
    // ROM with INHIBIT asserted -- in both cases, a card on either
    // connector gets first refusal before we fall back to the open-bus
    // 0xFF. Consulted on every access in these ranges, not a memory-map
    // shortcut.
    uint8_t v;
    if (m_expansionConnector && m_expansionConnector->read(addr, m_pu, m_pv, v)) return v;
    if (m_systemBus && m_systemBus->read(addr, m_pu, m_pv, v)) return v;
    return 0xFF;
}

uint8_t PC1500Memory::readME0(uint16_t addr) {
    if (const uint8_t* p = resolve(addr, /*forWrite=*/false)) return *p;
    return readOpenBus(addr);
}

void PC1500Memory::writeME0(uint16_t addr, uint8_t value) {
    if (uint8_t* p = resolve(addr, /*forWrite=*/true)) { *p = value; return; }
    if (m_expansionConnector && m_expansionConnector->write(addr, m_pu, m_pv, value)) return;
    if (m_systemBus && m_systemBus->write(addr, m_pu, m_pv, value)) return;
}

uint8_t PC1500Memory::peek(uint16_t addr) const {
    if (const uint8_t* p = resolve(addr, /*forWrite=*/false)) return *p;
    return readOpenBus(addr);
}

bool PC1500Memory::debugSlotResponds(uint16_t addr) const {
    uint8_t v;
    return m_expansionConnector && m_expansionConnector->read(addr, m_pu, m_pv, v);
}

void PC1500Memory::poke(uint16_t addr, uint8_t value) {
    // Same address decode as writeME0(), but flagged `direct` on the
    // connector path: this is the host/debug/preset-loader write, not a
    // guest-CPU store, so a card that gates runtime writes (the CE-163F's
    // flash banks need a JEDEC unlock sequence) lets it through
    // unconditionally. The preset loader has no concept of a bank or a lock
    // -- it just pokes the currently-selected bank -- which is exactly the
    // semantics a debug poke wants too.
    if (uint8_t* p = resolve(addr, /*forWrite=*/true)) { *p = value; return; }
    if (m_expansionConnector &&
        m_expansionConnector->write(addr, m_pu, m_pv, value, /*direct=*/true)) return;
    // SystemBus (60-pin) carries no lock-gating card today, so a plain
    // write is enough -- PinState::direct defaults false, matching the
    // pre-`direct` behavior.
    if (m_systemBus && m_systemBus->write(addr, m_pu, m_pv, value)) return;
}

uint8_t PC1500Memory::readME1(uint16_t addr) {
    // A CE-150 (or any 60-pin card) claiming the LH5810 window shadows the
    // internal I/O chip that also aliases it -- see kCe150IoBase's comment.
    if (m_systemBus && addr >= kCe150IoBase && addr <= kCe150IoEnd) {
        uint8_t v;
        if (m_systemBus->readME1(addr, m_pu, m_pv, v)) return v;
    }
    if (isIoChipAddress(addr)) {
        switch (addr & 0xF) { // RS0-3 = AD0-3
            case 0xC: return m_dda;
            case 0xE: return m_opa;
            case 0xD: return m_ddb;
            case 0xF: {
                uint8_t v = uint8_t(m_opb | 0x08); // PB3 must read high -- confirmed ROM-dispatch gotcha, see header comment
                if (m_rtc.tp()) v |= 0x20; else v &= uint8_t(~0x20); // PB5 = RTC TP live level (+ any pending edge) -- see Upd1990ac::tp()
                if (m_rtc.dataOut()) v |= 0x40; else v &= uint8_t(~0x40); // PB6 = RTC DATA OUT -- the bit WRITE_2_CLOCK ($E52B) clocks out on a TIME read
                if (m_onKeyPressed) v &= uint8_t(~0x80); else v |= 0x80; // ON key on PB7; polarity unconfirmed, see header comment
                return v;
            }
            case 0x8: return m_opc;
            case 0xB:
                // TP's rising edge (if any) latches into bit 1 here --
                // deliberately does NOT clear on read, see m_if's own doc
                // comment in the header for why that specific behavior is
                // what actually breaks BREAK.
                if (m_rtc.consumeRisingEdge()) m_if |= 0x02;
                return m_if;
            default: return m_ioScratchRegs[addr & 0xF]; // serial/etc: not modeled, but read back what was written
        }
    }
    // Only the 60-pin SystemBus exposes DME1/ME1 (the 40-pin connector has
    // no equivalent) -- give it first refusal before falling back to the
    // ME0 mirror, resolving this file's earlier "flagged for revisit once
    // Phase 4's ExpansionConnector work clarifies ME1's remaining role."
    if (m_systemBus) {
        uint8_t v;
        if (m_systemBus->readME1(addr, m_pu, m_pv, v)) return v;
    }
    return readME0(addr); // no other documented ME1 wiring -- conservative mirror
}

void PC1500Memory::writeME1(uint16_t addr, uint8_t value) {
    // See readME1(): a 60-pin card claiming the LH5810 window gets it first.
    if (m_systemBus && addr >= kCe150IoBase && addr <= kCe150IoEnd) {
        if (m_systemBus->writeME1(addr, m_pu, m_pv, value)) return;
    }
    if (isIoChipAddress(addr)) {
        switch (addr & 0xF) {
            case 0xC: m_dda = value; return;
            case 0xE: m_opa = value; return; // drives the keyboard column strobe -- see readInputPort()
            case 0xD: m_ddb = value; return;
            case 0xF: m_opb = value; return;
            case 0x8:
                m_opc = value;
                // PC0=DATA IN, PC1=STB, PC2=CLK, PC3-5=C0-C2 -- see
                // Upd1990ac::setControlPins(). All six lines matter: STB
                // latches the C0/C1/C2 mode select, CLK shifts the 40-bit
                // calendar register, DATA IN feeds bit 39 on a TIME= write.
                m_rtc.setControlPins((value & 0x01) != 0, (value & 0x02) != 0,
                                      (value & 0x04) != 0, (value & 0x08) != 0,
                                      (value & 0x10) != 0, (value & 0x20) != 0);
                // PC6 drives the piezo buzzer; the BEEP loop toggles it.
                m_piezo.setLevel((value & 0x40) != 0);
                return;
            case 0xB: m_if = value; return;
            default: m_ioScratchRegs[addr & 0xF] = value; return; // serial/etc: not modeled, but not discarded either
        }
    }
    if (m_systemBus && m_systemBus->writeME1(addr, m_pu, m_pv, value)) return;
    writeME0(addr, value);
}

uint8_t PC1500Memory::readInputPort() {
    // A PA bit only actually drives its column line when DDA has configured
    // it as an output; an input-mode bit floats regardless of what's latched
    // in OPA, so it must never read as "strobed" (see m_dda's doc comment in
    // the header for why this matters -- the real per-key scan routine
    // selects one column at a time entirely via DDA, holding OPA at a fixed
    // 0x00 the whole time). scan()'s driveLines convention is active-low
    // (0 = strobed), so an
    // input-mode bit (DDA=0) must be forced to 1 ("not strobed") regardless
    // of OPA -- exactly what ORing in ~m_dda does.
    uint8_t effectiveDriveLines = uint8_t(m_opa | uint8_t(~m_dda));
    return m_keyboard.scan(effectiveDriveLines);
}
