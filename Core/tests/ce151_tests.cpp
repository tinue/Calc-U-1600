// Headless C++ tests for the CE-151 (4 KB) memory module, run against the
// bundled definition Qt6/resources/cards/ce151.card.yaml
// (SoftwareDefinedCard). Same no-framework, assert-and-tally style as the
// other Core tests.
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

PinState makePins(uint16_t addr) {
    PinState p;
    p.address = addr;
    return p;
}

// Declares `card` (a fresh CE-151). The definition ships in the repo, so a
// missing or unloadable one fails the test.
#define CE151_OR_FAIL(card, host)                                                         \
    auto card = bundledCard("ce151.card.yaml", host);                                     \
    if (!card) {                                                                          \
        g_fail++; std::fprintf(stderr, "FAIL %s: Qt6/resources/cards/ce151.card.yaml missing or unloadable\n", __func__); \
        return;                                                                           \
    }

// Two 2 KB chips, CS = pin 16 (S1) and pin 17 (S2); A0-A10 are the offset.
void test_ce151_pins_16_17_chips() {
    CE151_OR_FAIL(owned, CardHost::PC1500);
    SoftwareDefinedCard& card = *owned;
    uint8_t v;

    PinState c1lo = makePins(0x4800); c1lo.pin[16] = true;
    PinState c1hi = makePins(0x4FFF); c1hi.pin[16] = true;
    PinState c2lo = makePins(0x5000); c2lo.pin[17] = true;
    PinState c2hi = makePins(0x57FF); c2hi.pin[17] = true;
    CHECK(card.respondsToWrite(c1lo, 0x11));
    CHECK(card.respondsToWrite(c1hi, 0x12));
    CHECK(card.respondsToWrite(c2lo, 0x21));
    CHECK(card.respondsToWrite(c2hi, 0x22));
    CHECK(card.respondsToRead(c1lo, v) && v == 0x11);
    CHECK(card.respondsToRead(c1hi, v) && v == 0x12);
    CHECK(card.respondsToRead(c2lo, v) && v == 0x21);
    CHECK(card.respondsToRead(c2hi, v) && v == 0x22);

    // No Y0 path: pin 4 with A11-A13 = 111 is not claimed (that is CE-155's
    // on-module decoder, which the CE-151 doesn't have).
    PinState y0 = makePins(0x3800); y0.pin[4] = true;
    CHECK(!card.respondsToRead(y0, v));
    // Pin 18 (S3) and pin 5 are not wired.
    PinState p18 = makePins(0x5800); p18.pin[18] = true;
    CHECK(!card.respondsToRead(p18, v));
    PinState p5 = makePins(0x6000); p5.pin[5] = true;
    CHECK(!card.respondsToRead(p5, v));
    PinState bare = makePins(0x4800);
    CHECK(!card.respondsToRead(bare, v));
}

// Through the machine: &4800-&57FF on the PC-1500 (pins 16/17 = S1/S2),
// &5800-&67FF on the PC-1500A (pins 16/17 = S3/S4).
void test_ce151_end_to_end_via_machine() {
    CE151_OR_FAIL(card, CardHost::PC1500);
    PC1500Machine pc1500(PC1500Variant::PC1500);
    pc1500.attachExpansionCard(std::move(card));
    CHECK(pc1500.expansionConnector().attachedCard() &&
          pc1500.expansionConnector().attachedCard()->moduleName() == "CE-151");
    pc1500.memory().writeME0(0x4800, 0x77);
    pc1500.memory().writeME0(0x57FF, 0x78);
    CHECK(pc1500.memory().readME0(0x4800) == 0x77);
    CHECK(pc1500.memory().readME0(0x57FF) == 0x78);
    pc1500.memory().writeME0(0x3800, 0x66);
    CHECK(pc1500.memory().readME0(0x3800) != 0x66); // no RAM below &4000
    pc1500.memory().writeME0(0x5800, 0x55);
    CHECK(pc1500.memory().readME0(0x5800) != 0x55); // nor above &57FF

    CE151_OR_FAIL(cardA, CardHost::PC1500A);
    PC1500Machine pc1500a(PC1500Variant::PC1500A);
    pc1500a.attachExpansionCard(std::move(cardA));
    pc1500a.memory().writeME0(0x5800, 0x88);
    pc1500a.memory().writeME0(0x67FF, 0x89);
    CHECK(pc1500a.memory().readME0(0x5800) == 0x88);
    CHECK(pc1500a.memory().readME0(0x67FF) == 0x89);
    pc1500a.memory().writeME0(0x6800, 0x44);
    CHECK(pc1500a.memory().readME0(0x6800) != 0x44);
}

// The definition is declared for the PC-1500 family only.
void test_ce151_hosts() {
    CHECK(bundledCard("ce151.card.yaml", CardHost::PC1500) != nullptr);
    CHECK(bundledCard("ce151.card.yaml", CardHost::PC1500A) != nullptr);
    CHECK(bundledCard("ce151.card.yaml", CardHost::PC1600Slot1) == nullptr);
    CHECK(bundledCard("ce151.card.yaml", CardHost::PC1600Slot2) == nullptr);
}

} // namespace

int run_ce151_tests() {
    test_ce151_pins_16_17_chips();
    test_ce151_end_to_end_via_machine();
    test_ce151_hosts();

    std::printf("ce151_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
