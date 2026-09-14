#pragma once
#include <array>
#include <cstdint>

#include "ExpansionCard.hpp"

// ── CE-155 8KB RAM module ───────────────────────────────────────────────
//
// A plain, unbanked 8KB RAM module: four independently-chip-selected 2KB
// SRAM chips, no bank latch, no PU/PV gating, no INHIBIT
// (PC-1500-Address-Decoding.md §3.2, Software-Defined-Memory-Extension.md
// §2). It decodes purely from its own edge-connector pins, so the same
// object plugs into a PC-1500 ExpansionConnector or a PC-1600
// MemorySlotConnector unchanged -- whatever address it ends up answering at
// is a consequence of what the host drives onto those pins, exactly as on
// real hardware.
//
//   offset 0x0000-0x07FF   pin 4 asserted (CS) AND address bits A11-A13 = 111
//   offset 0x0800-0x0FFF   pin 16 asserted
//   offset 0x1000-0x17FF   pin 17 asserted
//   offset 0x1800-0x1FFF   pin 18 asserted
//
// On a PC-1500 pin 4 carries Y0 (&0000-&3FFF) and pins 16/17/18 carry
// S1/S2/S3 (&4800-&5FFF); on a PC-1500A the same three pins carry S3/S4/S5
// (&5800-&6FFF). The card is identical in both cases -- only the connector's
// pin-routing table differs. There is no host/variant branch here to get
// wrong.
class CE155Card : public ExpansionCard {
public:
    CE155Card() { m_ram.fill(0xFF); }

    bool respondsToRead(const PinState& pins, uint8_t& outValue) const override {
        int off = regionOffset(pins);
        if (off < 0) return false;
        outValue = m_ram[static_cast<size_t>(off)];
        return true;
    }

    bool respondsToWrite(const PinState& pins, uint8_t value) override {
        int off = regionOffset(pins);
        if (off < 0) return false;
        m_ram[static_cast<size_t>(off)] = value;
        return true;
    }

    std::vector<uint8_t> debugImage() const override {
        return std::vector<uint8_t>(m_ram.begin(), m_ram.end()); // 8 KB, four 2 KB chips
    }

private:
    std::array<uint8_t, 0x2000> m_ram{}; // 8KB, four 2KB chips concatenated (see class doc)

    // A 2KB-aligned chip-select strobe means the low 11 address bits are the
    // within-chip offset regardless of which address window the host's
    // strobe covers (&4800.. on a PC-1500, &5800.. on a PC-1500A, &8000..
    // on a PC-1600 slot).
    int regionOffset(const PinState& p) const {
        if (p.pin[4] && (p.address & 0x3800) == 0x3800) return p.address & 0x7FF;
        if (p.pin[16]) return 0x0800 + (p.address & 0x7FF);
        if (p.pin[17]) return 0x1000 + (p.address & 0x7FF);
        if (p.pin[18]) return 0x1800 + (p.address & 0x7FF);
        return -1;
    }
};
