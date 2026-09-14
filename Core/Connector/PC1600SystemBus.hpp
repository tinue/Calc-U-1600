#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

// ── PC-1600 60-pin system bus (CE-1600P and future peripherals) ──────────
//
// The PC-1600 has no published 60-pin TRM pinout the way the PC-1500 does
// (SystemBus.hpp's PC1500SignalDecode) -- PC1600Memory.hpp's own Page B
// comment notes banks 4/5 (this bus's ROM window) as real, documented
// peripheral content whose *pin table* is simply unresearched, only its
// bank/address behavior. Rather than force this onto ExpansionCard's
// PinState (a 40/60-pin-connector physical-contact model that has no slot
// for "which 16 KB ROM half" or "which Z-80 I/O port" -- PC1600's own
// concerns here), this bus defines its own narrow, honestly-scoped pin
// model covering only what the CE-1600P's protocol actually needs: a
// banked 16 KB ROM read window and a handful of I/O ports. A card decodes
// purely from `PC1600BusPins`, same
// "connector knows the host, card doesn't" split as the 40-pin family
// (ExpansionCard.hpp) -- it is just this bus's own pin vocabulary, not a
// physical edge-connector contact list.
//
// Chain-capable from the start (attach/detach/chain(), mirroring
// SystemBus.hpp's shape) even though only CE1600PCard exists today, so a
// later peripheral (e.g. the CE-1600F floppy add-on that docks onto a
// CE-1600P's own 50-pin sub-connector, or a from-scratch 60-pin device)
// does not require a bus rewrite.
struct PC1600BusPins {
    uint16_t address = 0; // ROM offset (romRead) or I/O port number (io)
    bool forWrite = false;
    bool io = false;    // true = I/O port access (IN/OUT); false = ROM read
    bool bank5 = false;  // Page B bank select for the ROM window: false =
                          // bank 4, true = bank 5.
};

class PC1600ExpansionCard {
public:
    virtual ~PC1600ExpansionCard() = default;

    /// Return true and set outValue if this card responds to this access.
    virtual bool respondsToRead(const PC1600BusPins& pins, uint8_t& outValue) const = 0;
    /// Return true if this card claims (and thus acts on) this write.
    virtual bool respondsToWrite(const PC1600BusPins& pins, uint8_t value) = 0;
};

class PC1600SystemBus {
public:
    void attach(PC1600ExpansionCard* card) {
        if (card && std::find(m_chain.begin(), m_chain.end(), card) == m_chain.end())
            m_chain.push_back(card);
    }
    void detach(PC1600ExpansionCard* card) {
        m_chain.erase(std::remove(m_chain.begin(), m_chain.end(), card), m_chain.end());
    }
    const std::vector<PC1600ExpansionCard*>& chain() const { return m_chain; }

    /// Page B banks 4/5 ROM window (Z-80 4000-7FFF); `offset` is address
    /// minus 0x4000. Consulted by PC1600Memory only when the page-B bank
    /// register actually selects 4 or 5, i.e. exactly where PC1600Memory's
    /// own resolveConst() already documents "deliberately left open bus
    /// until CE-1600P itself is emulated."
    bool readRom(uint16_t offset, bool bank5, uint8_t& outValue) const {
        if (m_chain.empty()) return false;
        PC1600BusPins pins;
        pins.address = offset;
        pins.bank5 = bank5;
        for (PC1600ExpansionCard* card : m_chain)
            if (card->respondsToRead(pins, outValue)) return true;
        return false;
    }

    bool readIO(uint8_t port, uint8_t& outValue) const {
        if (m_chain.empty()) return false;
        PC1600BusPins pins;
        pins.address = port;
        pins.io = true;
        for (PC1600ExpansionCard* card : m_chain)
            if (card->respondsToRead(pins, outValue)) return true;
        return false;
    }

    bool writeIO(uint8_t port, uint8_t value) {
        if (m_chain.empty()) return false;
        PC1600BusPins pins;
        pins.address = port;
        pins.io = true;
        pins.forWrite = true;
        for (PC1600ExpansionCard* card : m_chain)
            if (card->respondsToWrite(pins, value)) return true;
        return false;
    }

private:
    std::vector<PC1600ExpansionCard*> m_chain;
};
