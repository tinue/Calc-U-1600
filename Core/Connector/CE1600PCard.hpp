#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "AlpsPlotterMechanism.hpp"
#include "PC1600SystemBus.hpp"

// ── CE-1600P plotter, attached to the PC-1600's 60-pin system bus ───────
//
// This card claims:
//   - Page B banks 4/5 ROM read window (4000-7FFF, banked by PC1600BusPins
//     ::bank5) -- both 16 KB halves of PC1600-P1-B4-CE1600P.bin/-2.bin, loaded
//     contiguously.
//   - I/O port 0x82 write: Z-motor phase (low nibble) -- pen lift +
//     color-turret rotation.
//   - I/O port 0x83 write: X-motor phase (low nibble) / Y-motor phase
//     (high nibble).
//   - I/O port 0x81 read: bit5 = pen at left/home (penX() <= 0). Port 0x80
//     (key-interrupt enables) and the rest of 0x81 (paper-feed keys,
//     Print_Mode) have no host-side key events to report in this
//     emulation and are left unclaimed -- open bus there is
//     indistinguishable from "no key pressed," which is always the
//     correct answer with no on-screen plotter keypad.
//   - Ports 0x70-0x7F (CE-1600F floppy) and the write side of port 0x81
//     (FD reset) are claimed by a separate CE1600FCard, chained onto the
//     same PC1600SystemBus -- CE-1600F/P attach as a union
//     (PC1600Machine::attachCE1600P()), so the two cards are always
//     present together.
class CE1600PCard : public PC1600ExpansionCard {
public:
    static constexpr size_t kRomHalfSize = 0x4000;
    static constexpr size_t kRomSize = 2 * kRomHalfSize;

    /// `data` must be exactly kRomSize bytes: PC1600-P1-B4-CE1600P.bin followed by
    /// PC1600-P1-B5-CE1600P-OR-F.bin (16KB @ 0x0000, 16KB @ 0x4000 of the
    /// card's own 32KB address space).
    bool loadRom(const uint8_t* data, size_t size) {
        if (size != kRomSize) return false;
        std::memcpy(m_rom.data(), data, kRomSize);
        m_romLoaded = true;
        return true;
    }

    /// Same, but taking the two 16 KB halves separately (as
    /// PC1600Machine::attachCE1600P() receives them) -- copies each
    /// straight into place, no caller-side concatenation buffer needed.
    bool loadRom(const uint8_t* half1, size_t half1Size,
                 const uint8_t* half2, size_t half2Size) {
        if (half1Size != kRomHalfSize || half2Size != kRomHalfSize) return false;
        std::memcpy(m_rom.data(), half1, kRomHalfSize);
        std::memcpy(m_rom.data() + kRomHalfSize, half2, kRomHalfSize);
        m_romLoaded = true;
        return true;
    }

    AlpsPlotterMechanism&       mechanism() { return m_mechanism; }
    const AlpsPlotterMechanism& mechanism() const { return m_mechanism; }

    /// The whole loaded ROM (both 16 KB halves concatenated), for the GUI
    /// debug "Dump Mem" contents view -- empty if no ROM is loaded (should
    /// not happen once attached; PC1600Machine::attachCE1600P() always
    /// loads it first).
    std::vector<uint8_t> debugRomImage() const {
        if (!m_romLoaded) return {};
        return std::vector<uint8_t>(m_rom.begin(), m_rom.end());
    }

    bool respondsToRead(const PC1600BusPins& pins, uint8_t& outValue) const override {
        if (pins.io) {
            if (pins.address == 0x81) {
                outValue = m_mechanism.penX() <= 0 ? 0x20 : 0x00;
                return true;
            }
            return false;
        }
        if (!m_romLoaded) return false;
        const size_t offset = (pins.bank5 ? kRomHalfSize : 0) + (pins.address & 0x3FFF);
        outValue = m_rom[offset];
        return true;
    }

    bool respondsToWrite(const PC1600BusPins& pins, uint8_t value) override {
        if (!pins.io) return false; // ROM window: read-only
        switch (pins.address) {
            case 0x82: m_mechanism.writeMotorZ(value & 0x0F); return true;
            case 0x83:
                m_mechanism.writeMotorX(value & 0x0F);
                m_mechanism.writeMotorY((value >> 4) & 0x0F);
                return true;
            default: return false;
        }
    }

private:
    std::array<uint8_t, kRomSize> m_rom{};
    bool m_romLoaded = false;
    AlpsPlotterMechanism m_mechanism;
};
