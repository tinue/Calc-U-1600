// Headless C++ tests for the CE-155 (8 KB) memory module, run against the
// bundled definition Qt6/resources/cards/ce155.card.yaml
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

PinState makePins(uint16_t addr, bool y0 = false, bool y2 = false) {
    PinState p;
    p.address = addr;
    p.pin[4] = y0;   // Y0 chip select (&0000-&3FFF)
    p.pin[19] = y2;  // Y2 chip select (&8000-&BFFF)
    return p;
}

// Declares `card` (a fresh CE-155). The definition ships in the repo, so a
// missing or unloadable one fails the test.
#define CE155_OR_SKIP(card, host)                                                         \
    auto card = bundledCard("ce155.card.yaml", host);                                     \
    if (!card) {                                                                          \
        g_fail++; std::fprintf(stderr, "FAIL %s: Qt6/resources/cards/ce155.card.yaml missing or unloadable\n", __func__); \
        return;                                                                           \
    }

// The CE-155 decodes purely from its edge-connector pins: chip 0 on pin 4
// (with A11-A13 = 111), chips 1-3 on pins 16/17/18. It has no variant, so
// these tests drive the physical pins directly. Which address window those
// pins carry a strobe for is the connector's job, not the card's, and is
// covered by connector_tests.cpp.
void test_ce155_pin4_subrange_window() {
    CE155_OR_SKIP(owned, CardHost::PC1500);
    SoftwareDefinedCard& card = *owned;
    uint8_t v;

    // Chip 0: pin 4 asserted AND address bits A11-A13 = 111 (top 2KB of the
    // pin-4 window). Distinct bytes at the edges, no aliasing.
    PinState p3800 = makePins(0x3800, /*y0=*/true);
    PinState p3fff = makePins(0x3FFF, /*y0=*/true);
    CHECK(card.respondsToWrite(p3800, 0x11));
    CHECK(card.respondsToWrite(p3fff, 0x22));
    CHECK(card.respondsToRead(p3800, v) && v == 0x11);
    CHECK(card.respondsToRead(p3fff, v) && v == 0x22);

    // pin 4 asserted but A11-A13 != 111: not claimed.
    PinState p3799 = makePins(0x3799, /*y0=*/true);
    CHECK(!card.respondsToRead(p3799, v));

    // No strobe pin asserted at all: not claimed.
    PinState bare = makePins(0x4000);
    CHECK(!card.respondsToRead(bare, v));

    // pin 5 is not wired on this card (it is the connector's S4 / PVOUT
    // contact) -- a strobe there must not be claimed.
    PinState p5 = makePins(0x6000); p5.pin[5] = true;
    CHECK(!card.respondsToRead(p5, v));
}

void test_ce155_pins_16_17_18_chips() {
    CE155_OR_SKIP(owned, CardHost::PC1500);
    SoftwareDefinedCard& card = *owned;
    uint8_t v;

    // Chips 1/2/3 on pins 16/17/18 -- each a distinct 2KB region, offset by
    // the low 11 address bits regardless of which address the host strobes.
    PinState c1lo = makePins(0x4800); c1lo.pin[16] = true;
    PinState c1hi = makePins(0x4FFF); c1hi.pin[16] = true;
    PinState c2lo = makePins(0x5000); c2lo.pin[17] = true;
    PinState c3hi = makePins(0x5FFF); c3hi.pin[18] = true;
    CHECK(card.respondsToWrite(c1lo, 0x31));
    CHECK(card.respondsToWrite(c1hi, 0x32));
    CHECK(card.respondsToWrite(c2lo, 0x33));
    CHECK(card.respondsToWrite(c3hi, 0x34));
    CHECK(card.respondsToRead(c1lo, v) && v == 0x31);
    CHECK(card.respondsToRead(c1hi, v) && v == 0x32);
    CHECK(card.respondsToRead(c2lo, v) && v == 0x33);
    CHECK(card.respondsToRead(c3hi, v) && v == 0x34);

    // Same physical pins reached at a PC-1500A address window (&5800-&6FFF)
    // land on the same chips -- the card cannot tell the difference.
    CE155_OR_SKIP(ownedA, CardHost::PC1500A);
    SoftwareDefinedCard& cardA = *ownedA;
    PinState a1 = makePins(0x5800); a1.pin[16] = true;
    PinState a3 = makePins(0x6FFF); a3.pin[18] = true;
    CHECK(cardA.respondsToWrite(a1, 0x51));
    CHECK(cardA.respondsToWrite(a3, 0x53));
    CHECK(cardA.respondsToRead(a1, v) && v == 0x51);   // chip 1, offset 0x800
    CHECK(cardA.respondsToRead(a3, v) && v == 0x53);   // chip 3, offset 0x1FFF
}

void test_ce155_end_to_end_via_machine() {
    CE155_OR_SKIP(card, CardHost::PC1500);
    PC1500Machine pc1500(PC1500Variant::PC1500);
    pc1500.attachExpansionCard(std::move(card));
    // The slot reports which module sits in it (the definition's module-name).
    CHECK(pc1500.expansionConnector().attachedCard() &&
          pc1500.expansionConnector().attachedCard()->moduleName() == "CE-155");
    pc1500.memory().writeME0(0x4800, 0x77);
    CHECK(pc1500.memory().readME0(0x4800) == 0x77);
    pc1500.memory().writeME0(0x3800, 0x66);
    CHECK(pc1500.memory().readME0(0x3800) == 0x66);

    PC1500Machine pc1500a(PC1500Variant::PC1500A);
    pc1500a.attachExpansionCard(bundledCard("ce155.card.yaml", CardHost::PC1500A));
    pc1500a.memory().writeME0(0x5800, 0x88);
    CHECK(pc1500a.memory().readME0(0x5800) == 0x88);
    // On the PC-1500A, &4800 is built-in RAM -- resolve() claims it before
    // the connector, so the card never shadows it.
    pc1500a.memory().writeME0(0x4800, 0x99);
    CHECK(pc1500a.memory().readME0(0x4800) == 0x99); // built-in RAM answers, not the card
}

} // namespace

int run_ce155_tests() {
    test_ce155_pin4_subrange_window();
    test_ce155_pins_16_17_18_chips();
    test_ce155_end_to_end_via_machine();

    std::printf("ce155_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
