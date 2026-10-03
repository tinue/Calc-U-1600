// Headless C++ tests for the CE-161 (16 KB) memory module, run against the
// bundled definition Qt6/resources/cards/ce161.card.yaml
// (SoftwareDefinedCard). Same no-framework, assert-and-tally style as the
// other Core tests. The PC-1600 slot cases are in
// pc1600_slot_module_tests.cpp.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <string>

#include "../PC1500/PC1500Machine.hpp"
#include "TestCards.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

PinState makePins(uint16_t addr, bool y0) {
    PinState p;
    p.address = addr;
    p.pin[4] = y0; // Y0 chip select (&0000-&3FFF)
    return p;
}

#define CE161_OR_FAIL(card, host)                                                         \
    auto card = bundledCard("ce161.card.yaml", host);                                     \
    if (!card) {                                                                          \
        g_fail++; std::fprintf(stderr, "FAIL %s: Qt6/resources/cards/ce161.card.yaml missing or unloadable\n", __func__); \
        return;                                                                           \
    }

// Y0 alone selects all 16 KB; A0-A13 are the offset.
void test_ce161_pin4_full_window() {
    CE161_OR_FAIL(owned, CardHost::PC1500);
    SoftwareDefinedCard& card = *owned;
    uint8_t v;

    PinState lo = makePins(0x0000, true);
    PinState mid = makePins(0x2000, true);
    PinState hi = makePins(0x3FFF, true);
    CHECK(card.respondsToWrite(lo, 0x01));
    CHECK(card.respondsToWrite(mid, 0x02));
    CHECK(card.respondsToWrite(hi, 0x03));
    CHECK(card.respondsToRead(lo, v) && v == 0x01);
    CHECK(card.respondsToRead(mid, v) && v == 0x02);
    CHECK(card.respondsToRead(hi, v) && v == 0x03);

    // No strobe: not claimed. S-pins aren't wired.
    PinState bare = makePins(0x0000, false);
    CHECK(!card.respondsToRead(bare, v));
    PinState p16 = makePins(0x4800, false); p16.pin[16] = true;
    CHECK(!card.respondsToRead(p16, v));
}

// &0000-&3FFF on both models, right below the built-in RAM.
void test_ce161_end_to_end_via_machine() {
    for (PC1500Variant variant : {PC1500Variant::PC1500, PC1500Variant::PC1500A}) {
        const CardHost host = variant == PC1500Variant::PC1500 ? CardHost::PC1500 : CardHost::PC1500A;
        CE161_OR_FAIL(card, host);
        PC1500Machine m(variant);
        m.attachExpansionCard(std::move(card));
        CHECK(m.expansionConnector().attachedCard() &&
              m.expansionConnector().attachedCard()->moduleName() == "CE-161");
        m.memory().writeME0(0x0000, 0x5A);
        m.memory().writeME0(0x3FFF, 0xA5);
        CHECK(m.memory().readME0(0x0000) == 0x5A);
        CHECK(m.memory().readME0(0x3FFF) == 0xA5);
    }
}

void test_ce161_hosts() {
    CHECK(bundledCard("ce161.card.yaml", CardHost::PC1500) != nullptr);
    CHECK(bundledCard("ce161.card.yaml", CardHost::PC1500A) != nullptr);
    CHECK(bundledCard("ce161.card.yaml", CardHost::PC1600Slot1) != nullptr);
    CHECK(bundledCard("ce161.card.yaml", CardHost::PC1600Slot2) != nullptr);
}

} // namespace

int run_ce161_tests() {
    test_ce161_pin4_full_window();
    test_ce161_end_to_end_via_machine();
    test_ce161_hosts();

    std::printf("ce161_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
