#pragma once
#include <cstdint>

#include "ExpansionCard.hpp"
#include "PC1500SignalDecode.hpp"
#include "../PC1500/PC1500Variant.hpp"

// ── PC-1500/1500A 40-pin expansion connector ─────────────────────────────
//
// Models the 40-pin connector as a first-class object PC1500Memory
// consults on every access into a module-slot address range -- never a
// memory-map shortcut. Exactly one card slot, matching real hardware
// (the 40-pin connector never
// daisy-chains -- see SystemBus for the 60-pin connector, which does).
//
// With no card attached, every signal reads "not present" -- identical to
// the plain open-bus behavior. Concrete cards implement the ExpansionCard
// interface and attach here: the table-driven SoftwareDefinedCard, built
// from a .card.yaml definition and wired by PC1500PresetLoader.
class ExpansionConnector {
public:
    explicit ExpansionConnector(PC1500Variant variant) : m_variant(variant) {}

    void attach(ExpansionCard* card) { m_card = card; }
    void detach() { m_card = nullptr; }
    ExpansionCard* attachedCard() const { return m_card; }

    /// Consulted by PC1500Memory only for ME0 addresses whose own decode
    /// (resolve()) already determined are open bus (Y0, the variant's open
    /// S-range, Y2) or ROM-with-INHIBIT-asserted.
    bool read(uint16_t addr, bool pu, bool pv, uint8_t& outValue) const {
        if (!m_card) return false;
        return m_card->respondsToRead(decode(addr, /*forWrite=*/false, pu, pv), outValue);
    }

    /// `direct` = true when the write comes from PC1500Memory::poke() (the
    /// host/debug/preset-loader path); forwarded to the card via
    /// PinState::direct so a lock-gating card (CE-163F flash) can let it
    /// bypass its runtime write protocol.
    bool write(uint16_t addr, bool pu, bool pv, uint8_t value, bool direct = false) {
        if (!m_card) return false;
        PinState pins = decode(addr, /*forWrite=*/true, pu, pv);
        pins.direct = direct;
        return m_card->respondsToWrite(pins, value);
    }

    bool inhibitAsserted() const { return m_card && m_card->assertsInhibit(); }

private:
    PC1500Variant m_variant;
    ExpansionCard* m_card = nullptr;

    // Per-variant S-block -> physical-pin routing (Expansion-Connectors.md
    // §3.1). PC-1500: pins 16/17/18/5 carry S1/S2/S3/S4. PC-1500A: the same
    // four pins carry S3/S4/S5/-- (pin 5 dead). S5 never reaches the
    // PC-1500's 40-pin connector at all; S1/S2 never reach the PC-1500A's.
    // Returns the pin an S-block lands on for this model, or 0 if it doesn't
    // reach this connector.
    int sBlockPin(int s) const {
        if (m_variant == PC1500Variant::PC1500) {
            switch (s) { case 1: return 16; case 2: return 17; case 3: return 18; case 4: return 5; }
        } else {
            switch (s) { case 3: return 16; case 4: return 17; case 5: return 18; }
        }
        return 0;
    }

    PinState decode(uint16_t addr, bool forWrite, bool pu, bool pv) const {
        PinState pins = PC1500SignalDecode::basePinState(addr, forWrite, pu, pv);
        int pin = sBlockPin(PC1500SignalDecode::sBlockIndex(addr));
        if (pin != 0) pins.pin[pin] = true;
        return pins;
    }
};
