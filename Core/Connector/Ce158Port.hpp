#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "Ce158Card.hpp"

// ── A machine's CE-158 socket: the card (when attached) + its serial peer ─
//
// What PC1500Machine and PC1600Machine have in common about the CE-158:
// owning the card, the sticky host SerialLink it is wired to on every
// attach, ticking, reset and draining the Centronics output. The machines
// only add their bus hookup (the PC-1500's SystemBus, the PC-1600's LH5803
// side) and any exclusion rule around it. Not locked -- the owning machine
// calls it under its own mutex.
class Ce158Port {
public:
    /// Builds a reset card running on a `clockHz` tick() clock, wired to
    /// the current link, or nullptr if `rom` isn't a CE-158 image. The
    /// caller hooks it onto its bus and then hands it to install().
    std::unique_ptr<Ce158Card> build(const uint8_t* rom, size_t romSize, double clockHz) const {
        auto card = std::make_unique<Ce158Card>();
        if (!card->loadRom(rom, romSize)) return nullptr;
        card->setClockHz(clockHz);
        card->reset();
        card->setSerialLink(m_link);
        return card;
    }
    void install(std::unique_ptr<Ce158Card> card) { m_card = std::move(card); }
    /// Drops the card; the caller unhooks it from its bus first.
    void remove() { m_card.reset(); }

    Ce158Card* card() const { return m_card.get(); }
    bool attached() const { return m_card != nullptr; }

    /// Non-owning; kept across detach/attach, so the host PTY stays put
    /// while the user toggles the interface.
    void setSerialLink(SerialLink* link) {
        m_link = link;
        if (m_card) m_card->setSerialLink(link);
    }

    void tick(uint64_t cycles) { if (m_card) m_card->tick(cycles); }
    void reset() { if (m_card) m_card->reset(); }
    /// Empty when no card is attached.
    std::vector<uint8_t> drainParallelOutput() {
        return m_card ? m_card->drainParallelOutput() : std::vector<uint8_t>{};
    }

private:
    std::unique_ptr<Ce158Card> m_card;
    SerialLink* m_link = nullptr;
};
