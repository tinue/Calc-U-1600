#pragma once
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "ExpansionCard.hpp"
#include "MemoryCardDefinition.hpp"

// ── The one general-purpose expansion card ─────────────────────────────
//
// The one general-purpose module, configured per-target via a table: an
// ExpansionCard whose behaviour comes entirely from a parsed
// MemoryCardDefinition (docs/Memory-Card-Definition-Format.md) -- every
// memory module in this project is one of these, built from a .card.yaml.
//
// v1: Regular or Flash content (a JEDEC-style flash command decoder, see
// flashWrite()), single-kind or `by-bank`-split; Unbanked or
// trigger-based Banked. The definition loader still rejects rom/line-based
// before a card is built.

class SoftwareDefinedCard : public ExpansionCard {
public:
    explicit SoftwareDefinedCard(MemoryCardDefinition def) : m_def(std::move(def)) {
        for (const Region& r : m_def.regions) {
            RegionState st;
            st.def = &r;
            st.backing.resize(r.capacity);
            for (uint32_t b = 0; b < r.banking.bankCount; b++) {
                auto it = r.initialContentByBank.find(b);
                if (it != r.initialContentByBank.end()) {
                    std::copy(it->second.begin(), it->second.end(),
                              st.backing.begin() + size_t(b) * r.banking.bankSize);
                } else {
                    uint8_t fill = r.contentForBank(b).powerUpFill;
                    std::fill_n(st.backing.begin() + size_t(b) * r.banking.bankSize,
                               r.banking.bankSize, fill);
                }
            }
            m_regions.push_back(std::move(st));
        }
    }

    bool respondsToRead(const PinState& pins, uint8_t& outValue) const override {
        if (pins.ioWrite) return false;
        for (const RegionState& st : m_regions) {
            uint32_t off;
            if (!locate(st, pins, &off)) continue;
            outValue = st.backing[off];
            return true;
        }
        return false;
    }

    WriteResult respondsToWrite(const PinState& pins, uint8_t value) override {
        // 1. A port/pin write that latches a bank number.
        for (RegionState& st : m_regions) {
            if (!st.def->banked) continue;
            const Banking& b = st.def->banking;
            if (b.triggerKind == TriggerKind::IoPort) {
                if (pins.ioWrite && (pins.address & 0xFF) == b.triggerPort) {
                    st.bank = static_cast<int>(extractBits(value, b.sampledBits));
                    return WriteResult::taken();
                }
            } else {  // TriggerKind::Pin -- a memory-write strobe
                if (!pins.ioWrite && pins.pin[b.triggerPin]) {
                    st.bank = static_cast<int>(extractBits(pins.address, b.sampledBits));
                    return WriteResult::taken();
                }
            }
        }
        if (pins.ioWrite) return WriteResult::ignored();  // an I/O write that is not our trigger

        // 2. A data write into a mapped slice.
        for (RegionState& st : m_regions) {
            uint32_t off;
            if (!locate(st, pins, &off)) continue;
            const Region& r = *st.def;
            const RegionContent& c = r.contentForBank(uint32_t(st.bank));
            // A mask ROM takes no write from anyone -- not even a host poke.
            // Claimed, so the bus doesn't fall through to open bus.
            if (c.kind == ContentKind::Rom) return WriteResult::refused();
            if (c.kind == ContentKind::Flash) {
                if (pins.direct) {
                    store(st.backing[off], value);  // poke/preset loader: unconditional
                    return WriteResult::taken();
                }
                // off = bank*bankSize + windowOffset (see locate()) -- the flash command
                // decoder only ever sees the window-relative address.
                uint32_t bankBase = uint32_t(st.bank) * r.banking.bankSize;
                flashWrite(st, r, c.flash, off - bankBase, value);
                return WriteResult::taken();  // the chip took the command, even one that leaves the array untouched
            }
            bool protectedNow = c.hasWriteProtect && c.writeProtectDefaultProtected;
            if (!c.writable || protectedNow) return WriteResult::refused();  // the card claims the write either way
            store(st.backing[off], value);
            return WriteResult::taken();
        }
        return WriteResult::ignored();
    }

    // Test-only introspection.
    int currentBank(size_t region = 0) const {
        return region < m_regions.size() ? m_regions[region].bank : 0;
    }
    bool flashIdle(size_t region = 0) const {
        return region < m_regions.size() && m_regions[region].flash == FlashDecoderState::Idle;
    }
    const MemoryCardDefinition& definition() const { return m_def; }

    std::string moduleName() const override { return m_def.moduleName; }

    uint64_t contentRevision() const override { return m_contentRevision; }

    /// First banked region's current bank, for the GUI "Dump Mem" column
    /// label; -1 when no region banks (a purely unbanked definition).
    int debugCurrentBank() const override {
        const RegionState* st = firstBankedRegion();
        return st ? st->bank : -1;
    }

    /// First banked region's total bank count, for the GUI's "N-way paging"
    /// resource label; -1 when no region banks. Same region-selection rule
    /// as debugCurrentBank() (whichever region's the first banked one).
    int debugBankCount() const override {
        const RegionState* st = firstBankedRegion();
        return st ? static_cast<int>(st->def->banking.bankCount) : -1;
    }

    /// Every region's backing store concatenated in definition order, for
    /// the GUI "Dump Mem" contents view.
    std::vector<uint8_t> debugImage() const override {
        std::vector<uint8_t> out;
        size_t total = 0;
        for (const RegionState& st : m_regions) total += st.backing.size();
        out.reserve(total);
        for (const RegionState& st : m_regions)
            out.insert(out.end(), st.backing.begin(), st.backing.end());
        return out;
    }

    /// Write counterpart of debugImage(): overwrite `n` bytes at the
    /// concatenated backing offset `off`, walking regions in definition
    /// order. All-or-nothing -- returns false and writes nothing if the
    /// range would cross past the last region's end. A write that straddles
    /// two adjacent regions is split across their backings (the regions are
    /// contiguous in this address space by construction).
    bool debugImageWrite(size_t off, const uint8_t* data, size_t n) override {
        if (n == 0) return true;
        if (!data) return false;
        size_t total = 0;
        for (const RegionState& st : m_regions) total += st.backing.size();
        if (off > total || n > total - off) return false;
        if (touchesRom(off, n)) return false;  // ROM is read-only for this path too
        size_t base = 0;
        for (RegionState& st : m_regions) {
            const size_t regEnd = base + st.backing.size();
            if (off < regEnd) {
                const size_t local = off - base;
                const size_t take = std::min(n, st.backing.size() - local);
                std::copy(data, data + take, st.backing.begin() + local);
                ++m_contentRevision;
                data += take;
                n -= take;
                off += take;
                if (n == 0) return true;
            }
            base = regEnd;
        }
        return n == 0;
    }

private:
    // Stores `value` into a backing cell, counting it as a content change
    // only when the cell actually takes a new value (see contentRevision()).
    void store(uint8_t& cell, uint8_t value) {
        if (cell == value) return;
        cell = value;
        ++m_contentRevision;
    }

    // Whether [off, off+n) of the concatenated backing (debugImage()'s
    // address space) touches a byte of a `rom` range.
    bool touchesRom(size_t off, size_t n) const {
        size_t base = 0;
        for (const RegionState& st : m_regions) {
            const Region& r = *st.def;
            const size_t regEnd = base + st.backing.size();
            const size_t lo = std::max(off, base), hi = std::min(off + n, regEnd);
            if (lo < hi) {
                const size_t bankSize = r.banking.bankSize;
                for (size_t b = (lo - base) / bankSize; b <= (hi - 1 - base) / bankSize; ++b)
                    if (r.contentForBank(static_cast<uint32_t>(b)).kind == ContentKind::Rom) return true;
            }
            base = regEnd;
        }
        return false;
    }

    // One flash command decoder per
    // region (the "chip" sitting behind the region's bank/window latch,
    // independent of it -- see flashWrite()'s doc comment).
    enum class FlashDecoderState {
        Idle,
        Unlock1,      // saw unlock-sequence step 0
        Unlock2,      // saw unlock-sequence step 1
        ProgramArmed, // saw byte-program-command -- next write programs one byte
        EraseSetup,   // saw erase-setup-command
        EraseUnlock1, // saw unlock-sequence step 0 again
        EraseUnlock2, // saw unlock-sequence step 1 again -- next write is chip/sector-erase
    };

    struct RegionState {
        const Region* def = nullptr;
        std::vector<uint8_t> backing;
        int bank = 0;
        FlashDecoderState flash = FlashDecoderState::Idle;
    };

    // Shared region-selection rule for the debugCurrentBank()/debugBankCount()
    // pair above: whichever region is the first banked one, or nullptr if
    // this definition has none.
    const RegionState* firstBankedRegion() const {
        for (const RegionState& st : m_regions)
            if (st.def->banked) return &st;
        return nullptr;
    }

    static bool groupMatches(const EnableGroup& g, const PinState& p) {
        for (int pin : g.requireHigh)
            if (pin < 1 || pin > 40 || !p.pin[pin]) return false;
        for (int pin : g.requireLow)
            if (pin >= 1 && pin <= 40 && p.pin[pin]) return false;
        for (const auto& ab : g.addrBits)
            if (static_cast<int>((p.address >> ab.bit) & 1) != ab.level) return false;
        if (g.hasMemoryRange && (p.address < g.rangeFrom || p.address > g.rangeTo)) return false;
        return true;
    }

    static uint32_t extractBits(uint32_t source, const std::vector<int>& bits) {
        uint32_t v = 0;
        for (size_t i = 0; i < bits.size(); ++i)
            v |= ((source >> bits[i]) & 1u) << i;
        return v;
    }

    // A group's offset within its slice: a memory-range window counts from
    // its own `from` (it need be neither a power of two nor aligned), every
    // other group uses the low log2(span) address lines.
    static uint32_t groupOffset(const EnableGroup& g, const PinState& pins) {
        if (g.hasMemoryRange) return pins.address - g.rangeFrom;
        return pins.address & (g.span - 1);
    }

    // Resolve (region, pins) to a byte offset into the region's backing
    // store, or return false if this region does not claim the access.
    static bool locate(const RegionState& st, const PinState& pins, uint32_t* off) {
        const Region& r = *st.def;
        if (!r.banked) {
            for (const EnableGroup& g : r.addressing.groups) {
                if (!groupMatches(g, pins)) continue;
                *off = g.mapsTo + groupOffset(g, pins);
                return *off < st.backing.size();
            }
            return false;
        }
        // Banked: region addressing is a pure gate, then the bank window
        // gives the slice within the current bank.
        bool gated = r.addressing.groups.empty();
        for (const EnableGroup& g : r.addressing.groups) {
            if (groupMatches(g, pins)) { gated = true; break; }
        }
        if (!gated) return false;
        if (st.bank < 0 || static_cast<uint32_t>(st.bank) >= r.banking.bankCount) return false;
        for (const EnableGroup& g : r.banking.bankWindow.groups) {
            if (!groupMatches(g, pins)) continue;
            *off = static_cast<uint32_t>(st.bank) * r.banking.bankSize + g.mapsTo + groupOffset(g, pins);
            return *off < st.backing.size();
        }
        return false;
    }

    // The flash chip's command decoder (JEDEC style, as on the CE-163F's
    // SST39SF010A; addresses after `command-address-mask`):
    //
    //   (0x555,0xAA) (0x2AA,0x55) (0x555,0xA0) (addr,data)   byte program
    //   (0x555,0xAA) (0x2AA,0x55) (0x555,0x80)
    //     (0x555,0xAA) (0x2AA,0x55) (0x555,0x10)             chip erase
    //     (0x555,0xAA) (0x2AA,0x55) (sectorAddr,0x30)        sector erase
    //   (anywhere,0xF0)                                       reset to read
    //
    // NOR semantics: a program can only clear bits (array &= data); an erase
    // sets the affected bytes back to 0xFF. A write that doesn't match the
    // expected next step resets the decoder to Idle -- how real hardware
    // recovers from a glitched sequence. Erase and program complete
    // instantly (no DQ6/DQ7 status polling), so a read always returns the
    // array byte and firmware poll loops exit at once.
    //
    // `windowOffset` is the write's address relative
    // to the *current bank's* window (0..bankSize-1) -- never the region's
    // bank-latch trigger, which respondsToWrite() step 1 already handled
    // and returned from without touching `st.flash`: the command decoder
    // is its own state machine, independent of the bank-select latch, even
    // though real firmware interleaves writes to both in quick succession.
    void flashWrite(RegionState& st, const Region& r, const FlashProtocol& p,
                    uint32_t windowOffset, uint8_t data) {
        const uint32_t mask = p.commandAddressMask;
        const uint32_t cmd = windowOffset & mask;
        const uint32_t addr0 = p.unlockSequence[0].address & mask;
        const uint32_t addr1 = p.unlockSequence[1].address & mask;
        const uint8_t data0 = p.unlockSequence[0].data;
        const uint8_t data1 = p.unlockSequence[1].data;

        // A software reset returns the chip to read mode -- but ONLY while
        // it is awaiting a command. In ProgramArmed the chip is mid byte-load
        // cycle: the next write is the data + address, taken verbatim with no
        // command decode, so a data byte that happens to equal resetCommand
        // must still be programmed. The CE-163F firmware relies on this: its
        // `STA (DE) / CPA (DE) / JR NZ` verify poll would spin forever if a
        // 0xF0 data byte were swallowed as a reset.
        if (data == p.resetCommand && st.flash != FlashDecoderState::ProgramArmed) {
            st.flash = FlashDecoderState::Idle;
            return;
        }

        using S = FlashDecoderState;
        // An unbanked region is one "bank" of `capacity` bytes (normalised at parse time).
        const uint32_t bankSize = r.banking.bankSize;
        const uint32_t bankCount = r.banking.bankCount;
        uint32_t bankBase = uint32_t(st.bank) * bankSize;
        switch (st.flash) {
            case S::Idle:
                st.flash = (cmd == addr0 && data == data0) ? S::Unlock1 : S::Idle;
                return;
            case S::Unlock1:
                if (cmd == addr1 && data == data1) st.flash = S::Unlock2;
                else if (cmd == addr0 && data == data0) st.flash = S::Unlock1;
                else st.flash = S::Idle;
                return;
            case S::Unlock2:
                if (cmd == addr0 && data == p.byteProgramCommand) st.flash = S::ProgramArmed;
                else if (cmd == addr0 && data == p.eraseSetupCommand) st.flash = S::EraseSetup;
                else st.flash = S::Idle;
                return;
            case S::ProgramArmed:
                store(st.backing[bankBase + windowOffset],
                      uint8_t(st.backing[bankBase + windowOffset] & data));  // NOR: can only clear bits
                st.flash = S::Idle;
                return;
            case S::EraseSetup:
                st.flash = (cmd == addr0 && data == data0) ? S::EraseUnlock1 : S::Idle;
                return;
            case S::EraseUnlock1:
                st.flash = (cmd == addr1 && data == data1) ? S::EraseUnlock2 : S::Idle;
                return;
            case S::EraseUnlock2:
                if (cmd == addr0 && data == p.chipEraseCommand) {
                    // Clears the whole Flash content-range at once -- every
                    // flash-kind bank in this region, not just the current one.
                    for (uint32_t b = 0; b < bankCount; b++) {
                        if (r.contentForBank(b).kind != ContentKind::Flash) continue;
                        std::fill_n(st.backing.begin() + size_t(b) * bankSize, bankSize, uint8_t(0xFF));
                    }
                } else if (data == p.sectorEraseCommand) {
                    uint32_t base = windowOffset & ~(p.sectorSize - 1);
                    std::fill_n(st.backing.begin() + bankBase + base, p.sectorSize, uint8_t(0xFF));
                }
                ++m_contentRevision;
                st.flash = S::Idle;
                return;
        }
    }

    MemoryCardDefinition m_def;
    std::vector<RegionState> m_regions;
    uint64_t m_contentRevision = 0; // see contentRevision()
};

/// Build the universal card from a definition file for a specific target
/// host. Returns nullptr and fills `error` on a read/parse failure or when
/// the file's `compatible-hosts` does not list `targetHost`
/// (Memory-Card-Definition-Spec.md §1/§2). The card reports its
/// `module-name:` via moduleName().
inline std::unique_ptr<ExpansionCard> makeSoftwareDefinedCard(const std::string& specPath,
                                                              CardHost targetHost,
                                                              std::string* error) {
    std::ifstream in(specPath, std::ios::binary);
    if (!in) {
        *error = "cannot open module spec '" + specPath + "'";
        return nullptr;
    }
    std::stringstream ss;
    ss << in.rdbuf();

    MemoryCardDefinition def;
    if (!parseMemoryCardDefinition(ss.str(), &def, error)) {
        *error = "module spec '" + specPath + "': " + *error;
        return nullptr;
    }
    if (!def.compatibleWith(targetHost)) {
        *error = "module '" + def.moduleName + "' (" + specPath +
                 ") is not compatible with " + cardHostToken(targetHost);
        return nullptr;
    }
    return std::make_unique<SoftwareDefinedCard>(std::move(def));
}
