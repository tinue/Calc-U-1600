#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "../LH5801/LH5801.hpp"

// ── LH5803-side memory map ────────────────────────────────────────────
//
// Implements LH5801Bus with the LH5803's own plain, non-banked 64KB view
// of the machine -- a CPU-side reinterpretation of the same underlying
// storage the SC7852 sees banked:
//
//   0000-3FFF  Slot 1/2 module RAM (= Z-80's 8000-BFFF, whichever bank
//              Port 31H last selected) -- open bus, no memory module
//              is currently modeled.
//   4000-7FFF  fixed internal 16KB RAM (= Z-80's C000-FFFF Bank 0, no
//              banking). This is its own private 16KB array, not the
//              same backing store as PC1600Memory's page-D RAM; the two
//              CPUs do not yet share one physical RAM array, so the
//              LH5803 core boots standalone.
//   8000-BFFF  CE-158 ROM (PVOUT=1) or CE-150 ROM (PVOUT=0) window --
//              not backed with data, open bus.
//   C000-FFFF  internal ROM (PC1600-LH5803-C000-FFFF.bin), fixed, loadable.
//
// ME1 defaults to aliasing ME0 (LH5801Bus's own conservative default) --
// the ME0/ME1-as-two-distinct-64KB-areas nuance is not modeled; see
// LH5803.hpp's class comment for why.
class LH5803Memory : public LH5801Bus {
public:
    LH5803Memory() = default;

    /// Loads the 16KB internal ROM at C000-FFFF (PC1600-LH5803-C000-FFFF.bin). Returns
    /// false (untouched) if `size` isn't exactly 16384 bytes.
    bool loadROM(const uint8_t* data, size_t size);
    bool loadROMFile(const std::string& path);

    void reset(); // clears internal RAM only; ROM untouched

    // LH5801Bus
    uint8_t readME0(uint16_t addr) override;
    void    writeME0(uint16_t addr, uint8_t value) override;

    uint8_t peek(uint16_t addr) const;
    void    poke(uint16_t addr, uint8_t value);

private:
    static constexpr uint16_t kRamBase = 0x4000;
    static constexpr size_t   kRamSize = 0x4000; // 16384B
    static constexpr uint16_t kRomBase = 0xC000;
    static constexpr size_t   kRomSize = 0x4000; // 16384B

    std::array<uint8_t, kRamSize> m_internalRam{};
    std::array<uint8_t, kRomSize> m_rom{};
    bool m_romLoaded{false};
};
