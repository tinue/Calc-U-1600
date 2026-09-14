#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ExpansionCard.hpp"

// ── Plain unbanked RAM module ───────────────────────────────────────────
//
// A generic N-byte SRAM chip behind the slot's chip-select (pin 4). Not a
// specific Sharp product -- this is what MemorySlotConnector's size-based
// convenience (`attachSlot1(size_t)`) builds, standing in for a period
// module like the CE-1600M whose only on-board logic is "assert my RAM when
// pin 4 is asserted."
//
//   - pin 4 asserted (RAM1#/RAM2# from the mainboard) selects the chip.
//   - pin 5 (PVOUT) is the 16KB half-select: 0 -> low 16KB, 1 -> high 16KB
//     of the slot's two-bank window.
//   - An access whose resulting offset lands past the populated size reads
//     open bus (respondsToRead returns false) and ignores writes -- so a
//     16KB module in a 32KB (two-bank) window leaves its upper bank as
//     0xFF, exactly as an unpopulated bank does on real hardware.
class PlainRamCard : public ExpansionCard {
public:
    static constexpr size_t kHalfBank = 0x4000; // 16KB

    explicit PlainRamCard(size_t sizeBytes) : m_ram(sizeBytes, 0xFF) {}

    size_t size() const { return m_ram.size(); }
    std::vector<uint8_t> debugImage() const override { return m_ram; }
    bool debugImageWrite(size_t off, const uint8_t* data, size_t n) override {
        if (n == 0) return true;
        if (!data || off > m_ram.size() || n > m_ram.size() - off) return false;
        std::copy(data, data + n, m_ram.begin() + off);
        return true;
    }

    bool respondsToRead(const PinState& pins, uint8_t& outValue) const override {
        size_t off = offset(pins);
        if (off >= m_ram.size()) return false;
        outValue = m_ram[off];
        return true;
    }

    bool respondsToWrite(const PinState& pins, uint8_t value) override {
        size_t off = offset(pins);
        if (off >= m_ram.size()) return false;
        m_ram[off] = value;
        return true;
    }

private:
    std::vector<uint8_t> m_ram; // powers up 0xFF

    // A huge value when pin 4 isn't asserted, so the size check rejects it.
    size_t offset(const PinState& p) const {
        if (!p.pin[4]) return SIZE_MAX;
        return (p.pin[5] ? kHalfBank : 0) + (p.address & 0x3FFF);
    }
};
