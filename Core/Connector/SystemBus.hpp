#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

#include "ExpansionCard.hpp"
#include "PC1500SignalDecode.hpp"
#include "../PC1500/PC1500Variant.hpp"

// ── PC-1500/1500A 60-pin connector (I/O expansion bus) ───────────────────
//
// Models the 60-pin connector (TRM §4-3-2) -- the port the CE-150 plugs
// into with its own 60-pin male
// connector, itself exposing a 64-pin female connector on its rear panel so
// a second peripheral (typically a CE-158) can daisy-chain behind it.
// Unlike the 40-pin ExpansionConnector (exactly one slot), this connector
// is a CHAIN: any number of cards can be attached, and each independently
// decides whether it responds to a given access via its own chip-select
// logic (PinState) -- exactly like real hardware, where two devices on the
// chain never both claim the same address.
//
// Exposes signals the 40-pin connector doesn't: ME1-space access (DME1/ME1,
// pins 58/59 -- the LH5801's second bank; see readME1/writeME1 and
// PinState::me1's own doc comment for why this path doesn't reuse the
// Y0/Y2/S-block decode), and named-but-unwired placeholder pins for
// CMTIN/CMTOUT (cassette FSK audio -- genuinely analog, out of scope until
// cassette support is built), WEX/W1 (external WAIT), INT, BFO/φOS
// (sub-timing signals). None of those are consulted by anything in this
// phase -- they exist here only so the connector's pin model stays honest
// about the full 60 pins.
//
// Assumption, not yet independently confirmed: the 60-pin connector's
// pinout is identical between PC-1500 and PC-1500A. Expansion-Connectors.md
// documents a pin-reassignment table for the 40-pin connector's S1-S4/S5
// pins (§3.1) but no equivalent table for the 60-pin connector, so all
// S1-S4 route unconditionally here on both models. Worth confirming against
// a second TRM scan if one surfaces (matching that document's own "worth
// double-checking" callouts elsewhere).
class SystemBus {
public:
    explicit SystemBus(PC1500Variant variant) : m_variant(variant) {}

    PC1500Variant variant() const { return m_variant; }

    /// Appends to the chain (no-op if already attached). Order doesn't
    /// affect correctness in practice -- each card's own chip-select decode
    /// is address/PU/PV-specific, so "first responder" and "the one whose
    /// decode actually matches" should always coincide; two cards
    /// responding to the same access is a real hardware bus conflict, not
    /// something this phase resolves.
    void attach(ExpansionCard* card) {
        if (card && std::find(m_chain.begin(), m_chain.end(), card) == m_chain.end()) {
            m_chain.push_back(card);
            if (card->mayAssertInhibit()) m_inhibitChain.push_back(card);
        }
    }
    void detach(ExpansionCard* card) {
        m_chain.erase(std::remove(m_chain.begin(), m_chain.end(), card), m_chain.end());
        m_inhibitChain.erase(std::remove(m_inhibitChain.begin(), m_inhibitChain.end(), card),
                             m_inhibitChain.end());
    }
    const std::vector<ExpansionCard*>& chain() const { return m_chain; }

    /// Consulted by PC1500Memory only for ME0 addresses whose own decode
    /// already determined are open bus, or ROM-with-INHIBIT-asserted.
    bool read(uint16_t addr, bool pu, bool pv, uint8_t& outValue) const {
        if (m_chain.empty()) return false; // common case: no card on this bus
        PinState pins = decode(addr, /*forWrite=*/false, pu, pv);
        for (ExpansionCard* card : m_chain) {
            if (card->respondsToRead(pins, outValue)) return true;
        }
        return false;
    }

    bool write(uint16_t addr, bool pu, bool pv, uint8_t value) {
        if (m_chain.empty()) return false;
        PinState pins = decode(addr, /*forWrite=*/true, pu, pv);
        for (ExpansionCard* card : m_chain) {
            if (card->respondsToWrite(pins, value)) return true;
        }
        return false;
    }

    /// ME1-space access -- only the 60-pin connector exposes DME1/ME1, so
    /// only SystemBus offers this; ExpansionConnector has no equivalent.
    /// `addr` is the full ME1-space address; callers (PC1500Memory) are
    /// responsible for excluding the LH5811 I/O-chip's own decode window
    /// before consulting this. Deliberately does NOT reuse the ME0-side
    /// Y0/Y2/S-block decode -- ME1's own sub-decode into named chip-select
    /// blocks isn't documented, so a card claiming ME1 space must key off
    /// `me1 && address` directly (see PinState::me1's doc comment).
    bool readME1(uint16_t addr, bool pu, bool pv, uint8_t& outValue) const {
        if (m_chain.empty()) return false;
        PinState pins = decodeME1(addr, /*forWrite=*/false, pu, pv);
        for (ExpansionCard* card : m_chain) {
            if (card->respondsToRead(pins, outValue)) return true;
        }
        return false;
    }
    bool writeME1(uint16_t addr, bool pu, bool pv, uint8_t value) {
        if (m_chain.empty()) return false;
        PinState pins = decodeME1(addr, /*forWrite=*/true, pu, pv);
        for (ExpansionCard* card : m_chain) {
            if (card->respondsToWrite(pins, value)) return true;
        }
        return false;
    }

    // Queried on every host-ROM fetch; see ExpansionCard::mayAssertInhibit().
    bool inhibitAsserted() const {
        for (ExpansionCard* card : m_inhibitChain) if (card->assertsInhibit()) return true;
        return false;
    }

private:
    PC1500Variant m_variant;
    std::vector<ExpansionCard*> m_chain;
    std::vector<ExpansionCard*> m_inhibitChain; // the cards in m_chain that may assert INHIBIT

    // S-block -> physical-pin routing. The 60-pin connector's own S-pin
    // positions aren't transcribed in the research corpus, so this assumes
    // the same contacts the 40-pin connector uses (S1/S2/S3 on 16/17/18,
    // S4 on 5) -- the same "assumed identical to the 40-pin" posture the
    // class comment already takes for the rest of this pinout, and no
    // per-variant routing gap is documented here, so it applies to both
    // models. S5 has no documented contact on either connector.
    static int sBlockPin(int s) {
        switch (s) { case 1: return 16; case 2: return 17; case 3: return 18; case 4: return 5; }
        return 0;
    }

    PinState decode(uint16_t addr, bool forWrite, bool pu, bool pv) const {
        PinState pins = PC1500SignalDecode::basePinState(addr, forWrite, pu, pv);
        int pin = sBlockPin(PC1500SignalDecode::sBlockIndex(addr));
        if (pin != 0) pins.pin[pin] = true;
        return pins;
    }

    PinState decodeME1(uint16_t addr, bool forWrite, bool pu, bool pv) const {
        PinState pins;
        pins.address = addr;
        pins.forWrite = forWrite;
        pins.pin[3] = pu; // PU
        pins.pin[2] = pv; // PV
        pins.me1 = true;  // pin[] chip-selects deliberately left false -- see PinState::me1
        return pins;
    }
};
