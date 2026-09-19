// Headless C++ tests for the CE-502B (Statistics) program module -- a 16 KB
// mask ROM -- run against the bundled definition
// Qt6/resources/cards/ce502b.card.yaml (SoftwareDefinedCard, `content:
// rom`). Same no-framework, assert-and-tally style as the other Core tests.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>

#include "../PC1500/PC1500Machine.hpp"
#include "TestCards.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// The definition ships in the repo, so a missing or unloadable one fails.
std::unique_ptr<SoftwareDefinedCard> ce502b(CardHost host) {
    auto c = bundledCard("ce502b.card.yaml", host);
    CHECK(c != nullptr);
    return c;
}

void test_ce502b_is_a_16k_rom_module() {
    auto card = ce502b(CardHost::PC1500A);
    if (!card) return;
    CHECK(card->moduleName() == "CE-502B");
    CHECK(card->definition().isRom());
    CHECK(card->debugImage().size() == 0x4000);
    // Not offered on the PC-1600: it's a PC-1500 program module.
    CHECK(bundledCard("ce502b.card.yaml", CardHost::PC1600Slot1) == nullptr);
}

void test_ce502b_end_to_end_via_machine() {
    auto card = ce502b(CardHost::PC1500);
    if (!card) return;
    PC1500Machine pc1500(PC1500Variant::PC1500);
    pc1500.attachExpansionCard(std::move(card));
    auto& m = pc1500.memory();

    // The program-module header at &0000: marker 55, BASIC top &00C5,
    // 16 KB, FF = LIST not allowed.
    const uint8_t header[8] = {0x55, 0x00, 0x00, 0xC5, 0x40, 0x00, 0x00, 0xFF};
    for (uint16_t i = 0; i < 8; i++) CHECK(m.readME0(i) == header[i]);
    // The BASIC program starts at &00C5 with line 90 (00 5A).
    CHECK(m.readME0(0x00C5) == 0x00 && m.readME0(0x00C6) == 0x5A);

    // Read-only for the CPU and for a host poke alike.
    m.writeME0(0x00C5, 0x12);
    m.poke(0x00C6, 0x34);
    CHECK(m.readME0(0x00C5) == 0x00 && m.readME0(0x00C6) == 0x5A);
}

} // namespace

int run_ce502b_tests() {
    test_ce502b_is_a_16k_rom_module();
    test_ce502b_end_to_end_via_machine();

    std::printf("ce502b_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
