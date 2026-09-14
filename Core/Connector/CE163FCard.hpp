#pragma once
#include <array>
#include <cstdint>

#include "ExpansionCard.hpp"

// ── CE-163F: 16-bank module, 8 RAM banks + 8 FLASH banks ────────────────
//
// THROWAWAY PROTOTYPE, in the same spirit as CE155Card / CE1638PlusCard --
// a hardcoded card for the Phase 4 connector layer, not the general Phase 7
// software-defined module. It goes one step past CE1638PlusCard: 16 banks
// instead of 8, and the upper half is FLASH rather than RAM.
//
//   - 16 banks of 16KB at Y0 (pin 4, &0000-&3FFF), 256KB total. Bank latch
//     is the same trigger-based mechanism CE1638PlusCard models: a write
//     strobe on physical pin 18 (S3 on the PC-1500, S5 on the PC-1500A --
//     same pin either way) latches a bank number sampled from address bits
//     A0-A3 of that same write. 16
//     real banks now, so no mod-wrap: A0-A3 map straight to banks 0-15. The
//     strobe write's data byte is discarded (control write); the pin is not
//     readable.
//   - Banks 0-7 are plain read/write RAM.
//   - Banks 8-F are FLASH. At guest runtime they are NOT freely writable: a
//     bare CPU store does nothing to the array. A byte program or a sector
//     erase must be preceded by the JEDEC unlock sequence a real Microchip
//     SST39SF010A wants. The exact bus-level protocol matches the CE-163F
//     firmware's own update routines (UNLOCK_CYCLE / SECTOR_ERASE /
//     BYTE_WRITE_VERIFY in its self-patch code).
//
// Flash command decode watches only the low 11 address bits of each write
// into the Y0 window while a flash bank is selected -- which is why the
// firmware's unlock addresses are &1555/&2AAA, not the datasheet's
// &5555/&2AAA (&1555 & 0x7FF == 0x555). Sequence:
//
//   (0x555,0xAA) (0x2AA,0x55) (0x555,0xA0) (addr,data)   byte program
//   (0x555,0xAA) (0x2AA,0x55) (0x555,0x80)
//     (0x555,0xAA) (0x2AA,0x55) (0x555,0x10)             chip erase
//     (0x555,0xAA) (0x2AA,0x55) (sectorAddr,0x30)        4KB sector erase
//   (anywhere,0xF0)                                       reset to read
//
// NOR semantics: a byte program can only clear bits (array &= data); an
// erase sets the affected bytes back to 0xFF. Any write into the Y0 window
// that doesn't match the expected next step resets the state machine to
// Idle -- how real hardware recovers from a glitched sequence.
//
// The command decoder belongs to the flash *chip*, which sits behind the
// module's bank/window latch -- the two are independent state machines
// (see the CE1638/CE163F reference doc). A bank-select strobe therefore
// must NOT touch the command decoder: the real firmware's unlock cycle
// interleaves writes to the module's bank/command ports (&6808/&6809 on
// PC-1500A) with the flash-address writes (&1555/&2AAA), e.g. LOADER_HELPER_3
// in updaterm-selfpatch.asm does
//   STA(&6809)  STA(&1555)  STA(&6808)  STA(&2AAA)
// per cycle. Resetting the decoder on the &6808/&6809 writes would wipe
// the unlock before &1555/&2AAA could advance it, the flash would never
// arm, the firmware's post-program `CPA (DE) / JR NZ` verify loop would
// never see its byte and would spin forever (observed: PC stuck at the
// 3-instr poll loop, all regs frozen).
//
// Power-up fill differs by bank kind, purely as an analysis aid: RAM banks
// (0-7) fill 0xFF (this project's SRAM power-up convention); FLASH banks
// (8-F) fill 0xAA (kFlashInitFill). A real erased flash cell is 0xFF, so
// this makes "never-touched flash" visually distinct from both an erased
// sector and freshly powered-up RAM in a memory dump.
//
// A write flagged PinState::direct (PC1500Memory::poke() -- the preset
// loader / debugger) bypasses all of the above and writes the array
// unconditionally, RAM or flash: the loader has no concept of a bank or a
// lock, it just pokes the currently-selected bank.
//
// PC-1600 memory slot: like CE155Card / CE1638PlusCard, the object plugs
// into a MemorySlotConnector pin-for-pin and decodes from pins alone. Its
// pin-4 chip select is &8000-&BFFF on a PC-1600 slot rather than a
// PC-1500's &0000-&3FFF Y0, so every access into the banked window is
// masked to the 16KB bank size (`address & (kBankSize - 1)`) before it
// indexes a bank or feeds the flash command decoder's low-11-bit match --
// a no-op on a PC-1500 (Y0 is already 0..&3FFF).
//
// Use PC-1600 *Slot 2*: pins 16-18 there carry the dormant K0-K2 lines, so
// the pin-18 bank strobe never fires and the card presents bank 0 (RAM) as
// a plain unbanked 16K -- which is how the real module behaves on a
// PC-1600, and the boot ROM sizes it as +16384 (BASIC RAM base C0C5H ->
// 80C5H). In Slot 1 pin 18 carries S3, pulsed on every &B000-&B7FF write,
// so the boot RAM-sizing scan trips the bank latch (walking it into the
// FLASH banks) and the module contributes nothing -- see
// docs/PC1600-Core-Limitations.md's expansion-connector section.
//
// Deliberately NOT modeled: DQ7/DQ6 toggle-bit status polling (erase and
// program complete instantaneously here, and a plain read of a flash bank
// always returns the current array byte, so the firmware's post-op poll
// loops -- `LDA (0000); AND 80; JR Z` after erase, `CPA (DE); JR NZ` after
// program -- exit naturally); software-ID / autoselect (0x90), which the
// CE163F firmware never issues. No disk persistence -- flash banks are
// volatile like every other module's storage in this project.
class CE163FCard : public ExpansionCard {
public:
    CE163FCard() {
        for (int b = 0; b < kBankCount; b++)
            m_banks[size_t(b)].fill(isFlashBank(b) ? kFlashInitFill : uint8_t(0xFF));
    }

    bool respondsToRead(const PinState& pins, uint8_t& outValue) const override {
        if (pins.pin[4]) {
            outValue = m_banks[m_bank][pins.address & (kBankSize - 1)];
            return true;
        }
        return false; // no unbanked side regions
    }

    bool respondsToWrite(const PinState& pins, uint8_t value) override {
        if (pins.pin[18]) {
            // Bank/window latch only -- deliberately does NOT touch m_flash
            // (the chip's command decoder sits behind this latch; the
            // firmware interleaves &6808/&6809 strobes with the &1555/&2AAA
            // unlock writes -- see the class doc comment).
            m_bank = pins.address & 0x0F; // 0..15, one bank per value -- no mod-wrap
            return true;                  // control write -- value discarded
        }
        if (pins.pin[4]) {
            const uint16_t off = uint16_t(pins.address & (kBankSize - 1)); // PC-1600 slot: window is &8000-&BFFF
            if (pins.direct || !isFlashBank(m_bank)) {
                m_banks[m_bank][off] = value;
                return true;
            }
            flashWrite(off, value);
            return true; // claimed even when the state machine leaves the array untouched
        }
        return false;
    }

    // Test-only introspection (mirrors CE1638PlusCard::currentBank()).
    int  currentBank() const { return m_bank; }
    bool flashIdle()   const { return m_flash == FlashState::Idle; }

    int debugCurrentBank() const override { return m_bank; } // GUI "Dump Mem" column label

    std::vector<uint8_t> debugImage() const override { // 16 banks x 16 KB (8 RAM, 8 FLASH)
        std::vector<uint8_t> out;
        out.reserve(kBankCount * kBankSize);
        for (const auto& bank : m_banks) out.insert(out.end(), bank.begin(), bank.end());
        return out;
    }

private:
    static constexpr int    kBankCount     = 16;
    static constexpr int    kFirstFlashBank = 8;
    static constexpr size_t kBankSize      = 0x4000; // 16KB, exactly the Y0 window
    static constexpr size_t kSectorSize    = 0x1000; // 4KB (SST39SF010A: 32 x 4K)

    // FLASH bank power-up fill -- deliberately not 0xFF (see class doc
    // comment): keeps never-touched flash distinct from erased flash in a
    // memory dump, an aid when analysing flash operations.
    static constexpr uint8_t kFlashInitFill = 0xAA;

    // Low-11-bit command addresses (see class doc comment).
    static constexpr uint16_t kCmdAddr1 = 0x555; // datasheet &5555, firmware &1555
    static constexpr uint16_t kCmdAddr2 = 0x2AA; // datasheet &2AAA, firmware &2AAA

    enum class FlashState {
        Idle,
        Unlock1,      // saw (0x555,0xAA)
        Unlock2,      // saw (0x2AA,0x55)
        ProgramArmed, // saw (0x555,0xA0) -- next write programs one byte
        EraseSetup,   // saw (0x555,0x80)
        EraseUnlock1, // saw (0x555,0xAA) again
        EraseUnlock2, // saw (0x2AA,0x55) again -- next write is 0x10 / 0x30
    };

    std::array<std::array<uint8_t, kBankSize>, kBankCount> m_banks{};
    int m_bank = 0;
    FlashState m_flash = FlashState::Idle;

    static bool isFlashBank(int bank) { return bank >= kFirstFlashBank; }

    void flashWrite(uint16_t addr, uint8_t data) {
        const uint16_t cmd = addr & 0x7FF; // chip decodes only the low 11 bits

        // Software reset (0xF0 to any address) returns the chip to read mode --
        // but ONLY while it is awaiting a command. In ProgramArmed the chip is
        // in the byte-load cycle of a program op: the next write is the data +
        // address, taken verbatim with no command decode, so a data byte that
        // happens to be 0xF0 must be programmed, not swallowed as a reset --
        // otherwise the firmware's post-write verify poll (`STA (DE) / CPA
        // (DE) / JR NZ`) would never see the byte it just wrote and would
        // spin forever.
        if (data == 0xF0 && m_flash != FlashState::ProgramArmed) {
            m_flash = FlashState::Idle;
            return;
        }

        switch (m_flash) {
            case FlashState::Idle:
                m_flash = (cmd == kCmdAddr1 && data == 0xAA) ? FlashState::Unlock1
                                                            : FlashState::Idle;
                return;
            case FlashState::Unlock1:
                if (cmd == kCmdAddr2 && data == 0x55) m_flash = FlashState::Unlock2;
                else if (cmd == kCmdAddr1 && data == 0xAA) m_flash = FlashState::Unlock1;
                else m_flash = FlashState::Idle;
                return;
            case FlashState::Unlock2:
                if (cmd == kCmdAddr1 && data == 0xA0) m_flash = FlashState::ProgramArmed;
                else if (cmd == kCmdAddr1 && data == 0x80) m_flash = FlashState::EraseSetup;
                else m_flash = FlashState::Idle;
                return;
            case FlashState::ProgramArmed:
                m_banks[m_bank][addr] &= data; // NOR: can only clear bits
                m_flash = FlashState::Idle;
                return;
            case FlashState::EraseSetup:
                m_flash = (cmd == kCmdAddr1 && data == 0xAA) ? FlashState::EraseUnlock1
                                                            : FlashState::Idle;
                return;
            case FlashState::EraseUnlock1:
                m_flash = (cmd == kCmdAddr2 && data == 0x55) ? FlashState::EraseUnlock2
                                                            : FlashState::Idle;
                return;
            case FlashState::EraseUnlock2:
                if (cmd == kCmdAddr1 && data == 0x10) {
                    for (int b = kFirstFlashBank; b < kBankCount; b++) m_banks[b].fill(0xFF);
                } else if (data == 0x30) {
                    uint16_t base = uint16_t(addr & ~(kSectorSize - 1));
                    for (size_t i = 0; i < kSectorSize; i++) m_banks[m_bank][base + i] = 0xFF;
                }
                m_flash = FlashState::Idle;
                return;
        }
    }
};
