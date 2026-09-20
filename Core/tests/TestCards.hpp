#pragma once
#include <cstddef>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "../Connector/MemoryCardDefinition.hpp"
#include "../Connector/SoftwareDefinedCard.hpp"

// Memory modules for the tests, built from definition files the same way
// the app builds them (SoftwareDefinedCard). Run from the repo root
// (tools/run_tests.sh): the bundled definitions live in
// Qt6/resources/cards/.

// A bundled module, e.g. bundledCard("ce163f.card.yaml", CardHost::PC1500A).
// nullptr when the file is missing or doesn't fit `host` -- the caller
// prints SKIP, same convention as the ROM helpers in TestRoms.hpp.
inline std::unique_ptr<SoftwareDefinedCard> bundledCard(const std::string& file, CardHost host) {
    std::ifstream in("Qt6/resources/cards/" + file, std::ios::binary);
    if (!in) return nullptr;
    std::stringstream ss;
    ss << in.rdbuf();
    MemoryCardDefinition def;
    std::string error;
    if (!parseMemoryCardDefinition(ss.str(), &def, &error) || !def.compatibleWith(host)) return nullptr;
    return std::make_unique<SoftwareDefinedCard>(std::move(def));
}

// Plain unbanked RAM for a PC-1600 memory slot, 16 KB or 32 KB -- a stand-in
// for "some RAM in the slot" (the CE-1600M is the 32 KB case). Pin 4 (the
// slot's chip select) selects it; PVOUT (pin 5) picks the low or high 16 KB,
// and a 16 KB module leaves the high half open bus.
inline std::unique_ptr<SoftwareDefinedCard> plainRamCard(size_t sizeBytes) {
    std::string yaml =
        "module-name: \"Test RAM\"\n"
        "compatible-hosts: [PC-1600-Slot-1, PC-1600-Slot-2]\n"
        "definition-terminology: PC-1600-Slot-1\n"
        "regions:\n"
        "  - name: ram\n"
        "    capacity: " + std::to_string(sizeBytes) + "\n"
        "    banking: none\n"
        "    content: regular\n"
        "    addressing:\n"
        "      any-of:\n"
        "        - all-of: [ { chip-select: RAM2 }, { signal-negated: PVOUT } ]\n"
        "          span: 0x4000\n"
        "          maps-to: 0x0000\n";
    if (sizeBytes > 0x4000)
        yaml +=
            "        - all-of: [ { chip-select: RAM2 }, { signal: PVOUT } ]\n"
            "          span: 0x4000\n"
            "          maps-to: 0x4000\n";
    MemoryCardDefinition def;
    std::string error;
    if (!parseMemoryCardDefinition(yaml, &def, &error)) return nullptr;
    return std::make_unique<SoftwareDefinedCard>(std::move(def));
}
