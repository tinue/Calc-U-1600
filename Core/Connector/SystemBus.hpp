#pragma once
#include <cstdint>
#include <vector>

#include "CardChain.hpp"
#include "SystemBusCard.hpp"

// ── The 60-pin connector (system / I/O expansion bus), both machines ─────
//
// One object per machine: PC1500Memory owns the PC-1500's, PC1600Memory the
// PC-1600's (driven by whichever CPU owns the bus). The CE-150 plugs into it
// and has a rear connector a second peripheral (typically a CE-158) chains
// onto; the CE-1600P likewise passes the bus on. So this is a CHAIN: every
// card sees every cycle and decides from the contacts whether it answers.
// Two cards answering the same cycle would be a bus conflict on real
// hardware; here the first one wins.
//
// The connector does no decoding of its own. The host builds the contacts
// of each cycle (SystemBusPins: PC1500SignalDecode::systemBusPins,
// PC1600BusDrive) and offers the cycle here; which cycles reach the plug at
// all is the host's address decode.
class SystemBus {
public:
    /// Appends to the chain (no-op if already attached).
    void attach(SystemBusCard* card) { m_chain.attach(card); }
    /// First in the chain: a bus ROM that shadows a bundled one (BusRomCard.hpp).
    void attachFirst(SystemBusCard* card) { m_chain.attachFirst(card); }
    void detach(SystemBusCard* card) { m_chain.detach(card); }
    const std::vector<SystemBusCard*>& chain() const { return m_chain.cards(); }
    bool empty() const { return m_chain.empty(); }

    bool read(const SystemBusPins& pins, uint8_t& outValue) const {
        return !m_chain.empty() && m_chain.read(pins, outValue);
    }
    WriteResult write(const SystemBusPins& pins, uint8_t value) {
        if (m_chain.empty()) return WriteResult::ignored();
        return m_chain.write(pins, value);
    }

    /// Whether a card says reading this cycle would disturb it -- debugger
    /// peeks only.
    bool readHasSideEffects(const SystemBusPins& pins) const {
        return !m_chain.empty() && m_chain.readHasSideEffects(pins);
    }

    // Queried on every host-ROM fetch; see InhibitSource.
    bool inhibitAsserted() const { return m_chain.inhibitAsserted(); }

    /// CMTOUT: the main unit's cassette-write line.
    void setCmtOut(bool level) { m_chain.setCmtOut(level); }
    /// CMTIN as a card drives it; `idle` if none does.
    bool cmtIn(bool idle) const { return m_chain.cmtIn(idle); }
    /// See SystemBusCard::advanceCassette / advanceTStates.
    void advanceCassette(uint32_t cycles) {
        for (SystemBusCard* card : m_chain.cards()) card->advanceCassette(cycles);
    }
    void advanceTStates(uint32_t tstates) {
        for (SystemBusCard* card : m_chain.cards()) card->advanceTStates(tstates);
    }

private:
    CardChain<SystemBusCard, SystemBusPins> m_chain;
};
