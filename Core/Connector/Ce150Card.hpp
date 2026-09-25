#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "AlpsPlotterMechanism.hpp"
#include "ExpansionCard.hpp"

// ── CE-150 printer / plotter / cassette interface (PC-1500 60-pin bus) ──
//
// The CE-150 has no CPU: its 8 KB firmware runs on the host LH5801,
// executing from a ROM window this card supplies. This card claims exactly
// two windows:
//
//   * ME0 read, guest 0xA000-0xBFFF, PV = 0            -> CE-150 system ROM
//   * ME1 read/write, guest 0xB008-0xB00F             -> LH5810 register
//     block: 0xB008 OPC, 0xB009 G, 0xB00A MSK, 0xB00B IF, 0xB00C DDA,
//     0xB00D DDB, 0xB00E OPA, 0xB00F OPB (register select = addr & 0x0F).
//
// The plotter has no move-to / select-colour command -- the firmware
// drives raw stepper phases through the LH5810: OPC low nibble = carriage
// (X) motor, OPC high nibble = paper-feed (Y) motor; OPB bit 0 / bit 1 =
// discrete pen up / down; the colour turret rotates mechanically when the
// carriage is driven to its left stop. `AlpsPlotterMechanism` holds that
// shared ALPS mechanism (see its header) -- this card only does the bus
// decode and the LH5810 latch/feedback model.
//
// Bus dependency -- deliberately narrow: `respondsToRead/Write` read only
// `pins.address`, `pins.forWrite`, `pins.me1` and `pins.pin[2]` (PV) via
// `decodeAccess()`. Nothing else. The PC-1500 `SystemBus` fills those four
// (`decode`/`decodeME1`); a PC-1600 `LH5803SharedMemory` (Phase 2) can
// hand-build the same four with no S-block/Y-strobe decode to reproduce.
//
// Cassette (CSAVE/CLOAD/CHAIN/PRINT#) is out of scope, same as the
// CE-1600P: the RMT bits on OPA are latched and ignored, the ROM's tape
// paths run but move no data.
class Ce150Card final : public ExpansionCard {
public:
    static constexpr size_t   kRomSize = 0x2000;   // 8192 B
    static constexpr uint16_t kRomBase = 0xA000;
    static constexpr uint16_t kRomEnd  = 0xBFFF;
    static constexpr uint16_t kIoBase  = 0xB008;   // LH5810 register block, ME1
    static constexpr uint16_t kIoEnd   = 0xB00F;

    /// `data` must be exactly `kRomSize` bytes (`CE-150.ROM`).
    bool loadRom(const uint8_t* data, size_t size) {
        if (size != kRomSize) return false;
        std::memcpy(m_rom.data(), data, kRomSize);
        m_romLoaded = true;
        return true;
    }

    AlpsPlotterMechanism&       mechanism() { return m_mechanism; }
    const AlpsPlotterMechanism& mechanism() const { return m_mechanism; }

    /// The loaded ROM, for the GUI debug "Dump Mem" view -- empty if no ROM
    /// is loaded (should not happen once attached).
    std::vector<uint8_t> debugRomImage() const {
        if (!m_romLoaded) return {};
        return std::vector<uint8_t>(m_rom.begin(), m_rom.end());
    }

    /// Re-anchor to power-on state: zero the LH5810 latches and reset the
    /// mechanism (which keeps the drawn strokes -- see its reset()).
    void reset() {
        m_opa = m_opb = m_opc = m_dda = m_ddb = m_g = m_msk = m_if = 0;
        m_mechanism.reset();
    }

    /// Per-emulation-step hook. A no-op today: the plotter is fully
    /// reactive (motion integrates on each OPC write). Reserved for the
    /// LH5810 tape-clock / paper-feed-key interrupt if those are ever
    /// modelled -- keeps `PC1500Machine::step()`/`runCycles()` from needing
    /// a second edit then.
    void tick(uint64_t /*cycles*/) {}

    bool respondsToRead(const PinState& pins, uint8_t& outValue) const override {
        const Access a = decodeAccess(pins);
        if (a.me1) {
            if (a.addr >= kIoBase && a.addr <= kIoEnd) {
                const uint8_t sel = static_cast<uint8_t>(a.addr & 0x0F);
                outValue = readReg(sel);
                return true;
            }
            return false;
        }
        if (a.forWrite || a.pv) return false;              // ROM: read-only, PV = 0 only
        if (a.addr < kRomBase || a.addr > kRomEnd) return false;
        if (!m_romLoaded) return false;
        outValue = m_rom[a.addr - kRomBase];
        return true;
    }

    WriteResult respondsToWrite(const PinState& pins, uint8_t value) override {
        const Access a = decodeAccess(pins);
        if (a.me1 && a.addr >= kIoBase && a.addr <= kIoEnd) {
            const uint8_t sel = static_cast<uint8_t>(a.addr & 0x0F);
            writeReg(sel, value);
            return WriteResult::taken();
        }
        return WriteResult::ignored();                     // ROM window read-only
    }

private:
    struct Access {
        uint16_t addr = 0;
        bool pv = false;
        bool me1 = false;
        bool forWrite = false;
    };
    static Access decodeAccess(const PinState& p) {
        return Access{p.address, p.pin[2] /*PV*/, p.me1, p.forWrite};
    }

    // LH5810 Port B read: the raw latch with the CE-150's live input bits
    // merged in, then masked to the input bits -- a Port B read only ever
    // exposes bits configured as inputs in the data-direction register
    // (`portB & ~ddb`).
    uint8_t portBRead() const {
        uint8_t v = m_opb;
        v = m_mechanism.colorMagnet() ? uint8_t(v | 0x04) : uint8_t(v & ~0x04); // PB2 colour magnet
        v &= uint8_t(~0x40); // PB6 low-battery = 0 (voltage OK)
        v &= uint8_t(~0x80); // PB7 paper-feed key = 0 (no on-screen keypad)
        return uint8_t(v & ~m_ddb);
    }

    uint8_t readReg(uint8_t sel) const {
        switch (sel) {
            case 0x8: return m_opc;
            case 0x9: return m_g;
            case 0xA: return m_msk;
            case 0xB: return m_if;
            case 0xC: return m_dda;
            case 0xD: return m_ddb;
            case 0xE: return uint8_t(m_opa & ~m_dda); // OPA read: input bits only
            case 0xF: return portBRead();
            default:  return 0xFF;
        }
    }

    void writeReg(uint8_t sel, uint8_t value) {
        switch (sel) {
            case 0x8: // OPC -- straight assignment, no data-direction masking
                m_opc = value;
                m_mechanism.writeCarriageMotorCE150(value & 0x0F);
                m_mechanism.writePaperMotorCE150(uint8_t((value >> 4) & 0x0F));
                return;
            case 0x9: m_g = value; return;
            case 0xA: m_msk = value; return;
            case 0xB: m_if = value; return;
            case 0xC: m_dda = value; return;
            case 0xD: m_ddb = value; return;
            case 0xE: // OPA -- write reaches output bits only; RMT lives here, latched/ignored
                m_opa = uint8_t((m_opa & ~m_dda) | (value & m_dda));
                return;
            case 0xF: // OPB -- write reaches output bits only; PB0/PB1 drive the pen
                m_opb = uint8_t((m_opb & ~m_ddb) | (value & m_ddb));
                m_mechanism.applyPenSignals(m_opb & 0x01, m_opb & 0x02);
                return;
            default: return;
        }
    }

    std::array<uint8_t, kRomSize> m_rom{};
    bool m_romLoaded = false;
    AlpsPlotterMechanism m_mechanism;

    // LH5810 register file (see the register-select table above).
    uint8_t m_opa = 0, m_opb = 0, m_opc = 0;
    uint8_t m_dda = 0, m_ddb = 0;
    uint8_t m_g = 0, m_msk = 0, m_if = 0;
};
