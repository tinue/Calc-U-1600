#pragma once
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "ExpansionCard.hpp"

// ── The card list behind every connector ─────────────────────────────────
//
// One shell for both plug types: which cards are plugged in, which of them
// can drive INHIBIT, and "offer this access to each card until one claims
// it". What the pins carry is the owning connector's business (its own
// decode()), never this class's -- it only dispatches an already-built pin
// state. `Card` is the card interface of that plug and `Pins` the pin state
// its respondsToRead/respondsToWrite take.
//
// A 60-pin connector is a real chain (attach() appends; the CE-150 exposes
// a rear connector a second peripheral plugs into). A 40-pin connector
// takes one module, which is a chain of one: replace() swaps the card.
//
// Order: first responder wins. Two cards claiming the same access is a bus
// conflict on real hardware, so attach order only matters in that case.
template <class Card, class Pins>
class CardChain {
public:
    using WriteReturn = decltype(std::declval<Card&>().respondsToWrite(std::declval<const Pins&>(), uint8_t{}));

    /// Appends (no-op if already attached, or null).
    void attach(Card* card) {
        if (!card || std::find(m_cards.begin(), m_cards.end(), card) != m_cards.end()) return;
        m_cards.push_back(card);
        if (auto* source = dynamic_cast<const InhibitSource*>(card)) m_inhibit.push_back(source);
    }
    void detach(Card* card) {
        m_cards.erase(std::remove(m_cards.begin(), m_cards.end(), card), m_cards.end());
        if (auto* source = dynamic_cast<const InhibitSource*>(card))
            m_inhibit.erase(std::remove(m_inhibit.begin(), m_inhibit.end(), source), m_inhibit.end());
    }
    /// The chain-of-one form: afterwards `card` (or nothing, for null) is
    /// the only card.
    void replace(Card* card) {
        clear();
        attach(card);
    }
    void clear() {
        m_cards.clear();
        m_inhibit.clear();
    }

    bool empty() const { return m_cards.empty(); }
    const std::vector<Card*>& cards() const { return m_cards; }
    /// The first (for a chain of one: the only) card, or null.
    Card* front() const { return m_cards.empty() ? nullptr : m_cards.front(); }

    bool read(const Pins& pins, uint8_t& outValue) const {
        for (Card* card : m_cards)
            if (card->respondsToRead(pins, outValue)) return true;
        return false;
    }
    /// The first claiming card's answer; a default-constructed one
    /// (WriteResult::ignored() / false) when nobody claims the write.
    WriteReturn write(const Pins& pins, uint8_t value) {
        for (Card* card : m_cards)
            if (WriteReturn r = card->respondsToWrite(pins, value)) return r;
        return WriteReturn{};
    }

    // Queried on every host-ROM fetch; see InhibitSource. Only the cards
    // that declared the capability are asked.
    bool inhibitAsserted() const {
        for (const InhibitSource* source : m_inhibit)
            if (source->assertsInhibit()) return true;
        return false;
    }

private:
    std::vector<Card*> m_cards;
    std::vector<const InhibitSource*> m_inhibit; // the cards in m_cards that can assert INHIBIT
};
