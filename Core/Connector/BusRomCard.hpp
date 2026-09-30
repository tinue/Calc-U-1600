#pragma once
#include <cstdint>
#include <utility>
#include <vector>

#include "ExpansionCard.hpp"
#include "PC1600SystemBus.hpp"

// ── A plain ROM module on a 60-pin expansion bus (preset `bus-rom:`) ──────
//
// For developing ROM extensions: a preset plugs a freshly assembled .bin in
// where a real ROM module would sit, and the next clean start (the preset
// applied again, a power-on) finds it. docs/Debugger.md.
//
// Both cards are ROM and nothing else: reads in their window, no writes, no
// I/O. A device that also has I/O (the host drive, the CE-158) keeps its own
// card; a bus ROM at the same place shadows that card's ROM, because the
// loader puts bus ROMs at the front of the chain (first responder wins). On
// real hardware two ROMs at one address would be a bus conflict; here it is
// how a rebuilt ROM replaces a bundled one.

/// PC-1500 60-pin connector (also the PC-1600's LH5803 half, which carries
/// the same signals): `base`..`base + size - 1` in ME0 or ME1, optionally
/// only for one PV / PU state (-1 = either), like the CE-150 (PV = 0) and
/// CE-158 (PV = 1, PU banked) ROMs.
class BusRomCard final : public ExpansionCard {
public:
    BusRomCard(std::vector<uint8_t> rom, uint16_t base, bool me1, int pv, int pu)
        : m_rom(std::move(rom)), m_base(base), m_me1(me1), m_pv(pv), m_pu(pu) {}

    bool respondsToRead(const PinState& pins, uint8_t& outValue) const override {
        if (pins.forWrite || pins.me1 != m_me1) return false;
        // PV/PU from their 40-pin contacts 2/3, the shortcut the CE-150 and
        // CE-158 use until the 60-pin connector is modelled (TODO.md).
        if (m_pv >= 0 && pins.pin[2] != (m_pv != 0)) return false;
        if (m_pu >= 0 && pins.pin[3] != (m_pu != 0)) return false;
        const uint32_t offset = uint32_t(pins.address) - m_base;
        if (pins.address < m_base || offset >= m_rom.size()) return false;
        outValue = m_rom[offset];
        return true;
    }
    WriteResult respondsToWrite(const PinState&, uint8_t) override { return WriteResult::ignored(); }

private:
    std::vector<uint8_t> m_rom;
    uint16_t m_base;
    bool m_me1;
    int m_pv, m_pu;
};

/// PC-1600 60-pin system bus: a ROM in page-1 bank `bank` (4-7), Z-80
/// 4000H up, like the CE-1600P (4/5) and the host drive (7).
class PC1600BusRomCard final : public PC1600ExpansionCard {
public:
    static constexpr size_t kBankSize = 0x4000;

    PC1600BusRomCard(std::vector<uint8_t> rom, uint8_t bank) : m_rom(std::move(rom)), m_bank(bank) {}

    bool respondsToRead(const PC1600BusPins& pins, uint8_t& outValue) const override {
        if (pins.io || pins.forWrite || pins.bank != m_bank || pins.address >= m_rom.size()) return false;
        outValue = m_rom[pins.address];
        return true;
    }
    bool respondsToWrite(const PC1600BusPins&, uint8_t) override { return false; }

private:
    std::vector<uint8_t> m_rom;
    uint8_t m_bank;
};
