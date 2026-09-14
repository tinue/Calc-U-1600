#pragma once
#include <array>
#include <cstdint>

#include "ExpansionCard.hpp"

// ── CE-1638+ 128KB banked RAM module: throwaway proof-of-concept
// ExpansionCard ────────────────────────────────────────────────────────
//
// THROWAWAY PROTOTYPE -- not a real Sharp module (no such product exists;
// the name is made up for this test card), built to exercise the connector
// layer's Y0 full-window banking path and a trigger-based bank latch,
// neither of which CE155Card (Y0 sub-range only, unbanked) exercises.
// Combines two independent pieces, hence the "+":
//
//   - A **banked** 16KB region at Y0 (&0000-&3FFF), 8 banks (128KB total),
//     modeled on CE-163's trigger-based mechanism
//     (Memory-Card-Definition-Spec.md §6): a write-strobe pin latches a
//     bank number sampled from the *address* lines of that same write, not
//     the data bus. The trigger pin is physical pin 18 -- which is S3 on
//     the PC-1500 and S5 on the PC-1500A (the same per-variant routing
//     CE155Card documents) -- and the bank number is read from address
//     bits A0-A3 of the triggering write. Only 8 banks exist, so the
//     latched 0-15 value is wrapped mod 8 (A3 is sampled but otherwise
//     redundant here; CE-163's own A0-only mechanism needs just one bit
//     for 2 banks, a hypothetical 16-bank extension would need all four --
//     this module's 8 banks sit in between).
//     The trigger write is a control write, not a data write -- it does
//     not store `value` anywhere, and the pin is not readable (matches
//     CE-163's write-strobe-only line; real hardware gives no meaningful
//     read response there either).
//   - Two independent, **unbanked** 2KB regions on pins 16/17 (S1/S2 on
//     the PC-1500, S3/S4 on the PC-1500A) -- plain RAM, same shape as
//     CE155Card's S1/S2/S3 regions, just fewer of them (pin 18 here is the
//     bank trigger, not a third unbanked region).
//
// Unlike the real CE-163 (Program Library, ROM/cassette-loaded), this
// throwaway models the banked region as plain read/write RAM -- simplest
// way to prove bank-switch dispatch end-to-end, not an attempt to match
// CE-163's actual read-only behavior.
//
// PC-1600 memory slot: the card plugs into a MemorySlotConnector unchanged
// (like CE155Card -- it decodes from pins, never from "which host"). There
// the pin-4 chip select covers &8000-&BFFF rather than a PC-1500's
// &0000-&3FFF Y0, so the banked-window index is masked to the 16KB bank
// size -- `address & (kBankSize - 1)` -- exactly as CE155Card reduces every
// strobe to a chip-local offset. On a PC-1500 that mask is a no-op (Y0 is
// already 0..&3FFF).
//
// Use PC-1600 *Slot 2*: pins 16-18 there carry the dormant K0-K2 lines, so
// the pin-18 bank strobe never fires, the pins 16/17 side regions are idle,
// and the card presents bank 0 as a plain unbanked 16K -- the boot ROM
// sizes it as +16384 (BASIC RAM base C0C5H -> 80C5H). In Slot 1 pin 18
// carries S3, pulsed on every &B000-&B7FF write, so the boot RAM-sizing
// scan keeps tripping the bank latch and the module contributes nothing --
// see docs/PC1600-Core-Limitations.md's expansion-connector section.
class CE1638PlusCard : public ExpansionCard {
public:
    CE1638PlusCard() {
        for (auto& bank : m_banks) bank.fill(0xFF);
        m_unbanked[0].fill(0xFF);
        m_unbanked[1].fill(0xFF);
    }

    bool respondsToRead(const PinState& pins, uint8_t& outValue) const override {
        if (pins.pin[4]) {
            outValue = m_banks[m_bank][pins.address & (kBankSize - 1)];
            return true;
        }
        int region = unbankedRegion(pins);
        if (region >= 0) {
            outValue = m_unbanked[region][pins.address & 0x7FF];
            return true;
        }
        return false;
    }

    bool respondsToWrite(const PinState& pins, uint8_t value) override {
        if (pins.pin[18]) {
            m_bank = (pins.address & 0x0F) % kBankCount;
            return true;
        }
        if (pins.pin[4]) {
            m_banks[m_bank][pins.address & (kBankSize - 1)] = value;
            return true;
        }
        int region = unbankedRegion(pins);
        if (region >= 0) {
            m_unbanked[region][pins.address & 0x7FF] = value;
            return true;
        }
        return false;
    }

    int currentBank() const { return m_bank; } // test-only introspection
    int debugCurrentBank() const override { return m_bank; } // GUI "Dump Mem" column label

    std::vector<uint8_t> debugImage() const override { // 8 banks x 16 KB, ascending
        std::vector<uint8_t> out;
        out.reserve(kBankCount * kBankSize);
        for (const auto& bank : m_banks) out.insert(out.end(), bank.begin(), bank.end());
        return out;
    }

private:
    static constexpr int kBankCount = 8;
    static constexpr size_t kBankSize = 0x4000; // 16KB, exactly the Y0 window

    std::array<std::array<uint8_t, kBankSize>, kBankCount> m_banks{};
    std::array<uint8_t, 0x800> m_unbanked[2]{}; // 2x2KB on pins 16/17
    int m_bank = 0;

    // Pins 16/17 -- S1/S2 on the PC-1500, S3/S4 on the PC-1500A: same two
    // physical pins either way, so no host branch.
    int unbankedRegion(const PinState& pins) const {
        if (pins.pin[16]) return 0;
        if (pins.pin[17]) return 1;
        return -1;
    }
};
