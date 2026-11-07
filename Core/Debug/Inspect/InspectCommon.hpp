#pragma once
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "../../Connector/ExpansionCard.hpp"
#include "../../Connector/SystemBusCard.hpp"
#include "HexDump.hpp"
#include "Inspector.hpp"

// ── What the PC-1500 and PC-1600 views share ─────────────────────────────
namespace inspect {

/// "8000–9FFF"; `digits` 5 or 6 for an offset into a large module.
inline std::string range(uint32_t lo, uint32_t hi, int digits = 4) {
    return fmt("%0*X–%0*X", digits, unsigned(lo), digits, unsigned(hi));
}

inline const char* kindName(CardMemory::Kind k) {
    switch (k) {
        case CardMemory::Kind::Rom: return "ROM";
        case CardMemory::Kind::Ram: return "RAM";
        case CardMemory::Kind::Flash: return "flash";
        case CardMemory::Kind::Mixed: return "mixed";
    }
    return "?";
}

/// A card's name as the views print it: "card" for one without a name.
inline std::string cardName(const CardBase& card) {
    std::string name = card.moduleName();
    return name.empty() ? "card" : name;
}

/// The names of the set bits of `v`, "—" for none.
inline std::string bits(uint8_t v, std::initializer_list<std::pair<int, const char*>> names) {
    std::string s;
    for (const auto& [bit, name] : names)
        if (v & (1u << bit)) s += (s.empty() ? "" : " ") + std::string(name);
    return s.empty() ? "—" : s;
}

inline std::string decodeOnOff(uint8_t v) { return v ? "on" : "off"; }

/// A run of `step`-byte blocks that scan()'s `answer` gives the same `who`.
template <class T>
struct Run {
    uint32_t lo, hi;
    T who;
};

/// `[lo, hi]` in `step`-byte blocks, merged into runs of the same
/// `answer(addr)`.
template <class F>
auto scan(uint32_t lo, uint32_t hi, uint32_t step, F answer) {
    using T = decltype(answer(uint16_t()));
    std::vector<Run<T>> runs;
    for (uint32_t a = lo; a <= hi; a += step) {
        T who = answer(uint16_t(a));
        if (!runs.empty() && runs.back().who == who && runs.back().hi + 1 == a)
            runs.back().hi = a + step - 1;
        else
            runs.push_back({a, a + step - 1, std::move(who)});
    }
    return runs;
}

/// Where a 60-pin card answers in the LH5801 / LH5803 ROM window 8000H-BFFFH,
/// found by offering it a read of each 8K half at every PU/PV: "8000–9FFF",
/// with " (PV=n)" when it answers at one PV only. `pins(addr, pu, pv)` builds
/// the host's contacts for an ME0 read.
template <class Pins>
std::vector<std::string> romWindowSeenAt(const SystemBusCard& card, Pins pins) {
    std::vector<std::string> where;
    for (uint16_t base : {uint16_t(0x8000), uint16_t(0xA000)}) {
        bool pvHit[2] = {false, false};
        for (int pv = 0; pv < 2; ++pv)
            for (int pu = 0; pu < 2; ++pu) {
                uint8_t v;
                if (card.respondsToRead(pins(base, pu != 0, pv != 0), v)) pvHit[pv] = true;
            }
        if (!pvHit[0] && !pvHit[1]) continue;
        where.push_back(range(base, base + 0x1FFFu) + (pvHit[0] != pvHit[1] ? fmt(" (PV=%d)", pvHit[1] ? 1 : 0) : ""));
    }
    return where;
}

/// `parts` joined with " · ".
inline std::string joinDots(const std::vector<std::string>& parts) {
    std::string s;
    for (const std::string& p : parts) s += (s.empty() ? "" : " · ") + p;
    return s;
}

/// A view's lines under its "── title ──" line.
inline std::vector<std::string> titled(View v, bool pc1500, std::vector<std::string> body) {
    body.insert(body.begin(), fmt("── %s ──", viewTitle(v, pc1500)));
    return body;
}

}  // namespace inspect
