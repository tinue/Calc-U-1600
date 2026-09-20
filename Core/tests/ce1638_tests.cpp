// Headless C++ tests for the CE-1638 banked-RAM module, run against the
// bundled definition Qt6/resources/cards/ce1638.card.yaml
// (SoftwareDefinedCard). Same no-framework, assert-and-tally style as the
// other Core tests.
//
// The card decodes purely from its edge-connector pins (banked window on
// pin 4, bank-latch strobe on pin 18), with no host/variant awareness -- so
// these tests drive the physical pins directly. Which address window a host
// drives those pins for is the connector's job and is covered by
// connector_tests.cpp.
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

// Declares `card` (a fresh CE-1638). The definition ships in the repo, so a
// missing or unloadable one fails the test.
#define CE1638_OR_SKIP(card, host)                                                        \
    auto card = bundledCard("ce1638.card.yaml", host);                                    \
    if (!card) {                                                                          \
        g_fail++; std::fprintf(stderr, "FAIL %s: Qt6/resources/cards/ce1638.card.yaml missing or unloadable\n", __func__); \
        return;                                                                           \
    }

PinState makePins(uint16_t addr, bool y0 = false) {
    PinState p;
    p.address = addr;
    p.pin[4] = y0; // pin 4 == Y0 bank window
    return p;
}

PinState triggerAt(uint16_t addr) { // bank-latch strobe on physical pin 18
    PinState p;
    p.address = addr;
    p.pin[18] = true;
    return p;
}

void test_ce1638_banking() {
    CE1638_OR_SKIP(owned, CardHost::PC1500);
    SoftwareDefinedCard& card = *owned;
    uint8_t v;

    // Bank 0 is selected at reset; write &0000/&3FFF distinctly.
    CHECK(card.currentBank() == 0);
    CHECK(card.respondsToWrite(makePins(0x0000, /*y0=*/true), 0x11));
    CHECK(card.respondsToWrite(makePins(0x3FFF, /*y0=*/true), 0x22));
    CHECK(card.respondsToRead(makePins(0x0000, /*y0=*/true), v) && v == 0x11);
    CHECK(card.respondsToRead(makePins(0x3FFF, /*y0=*/true), v) && v == 0x22);

    // A write strobe on pin 18 latches the bank from A0-A3 of the *write
    // address itself* (low nibble 5 -> bank 5) and does not touch bank
    // content -- the write's data byte is discarded.
    CHECK(card.respondsToWrite(triggerAt(0x4805), 0xEE)); // claimed, not stored as data
    CHECK(card.currentBank() == 5);

    // Bank 5 is a distinct 16KB region from bank 0: untouched, default fill.
    CHECK(card.respondsToRead(makePins(0x0000, /*y0=*/true), v) && v == 0x00);
    CHECK(card.respondsToWrite(makePins(0x0000, /*y0=*/true), 0x33));
    CHECK(card.respondsToRead(makePins(0x0000, /*y0=*/true), v) && v == 0x33);

    // Switch back to bank 0: bank 0's earlier content (0x11) is intact.
    CHECK(card.respondsToWrite(triggerAt(0x4800), 0x00)); // low nibble 0x0
    CHECK(card.currentBank() == 0);
    CHECK(card.respondsToRead(makePins(0x0000, /*y0=*/true), v) && v == 0x11);

    // Only A0-A2 are sampled (8 banks): low nibble 0xD (13) -> bank 5.
    CHECK(card.respondsToWrite(triggerAt(0x480D), 0x00));
    CHECK(card.currentBank() == 5);

    // The trigger pin itself is not readable (write-strobe only).
    CHECK(!card.respondsToRead(triggerAt(0x4800), v));
}

void test_ce1638_end_to_end_via_machine() {
    CE1638_OR_SKIP(card, CardHost::PC1500);
    PC1500Machine pc1500(PC1500Variant::PC1500);
    pc1500.attachExpansionCard(std::move(card));

    // Bank 0 (default).
    pc1500.memory().writeME0(0x0100, 0xAA);
    CHECK(pc1500.memory().readME0(0x0100) == 0xAA);

    // Trigger the bank switch via a real bus write -- pin 18 is S3 on the
    // PC-1500, so &5800-&5FFF.
    pc1500.memory().writeME0(0x5802, 0x00); // low nibble 0x2 -> bank 2
    pc1500.memory().writeME0(0x0100, 0xBB);
    CHECK(pc1500.memory().readME0(0x0100) == 0xBB);

    // Back to bank 0: original content intact.
    pc1500.memory().writeME0(0x5800, 0x00); // low nibble 0x0 -> bank 0
    CHECK(pc1500.memory().readME0(0x0100) == 0xAA);

    // Same card, PC-1500A: pin 18 is S5 there, so the trigger window moves
    // to &6800-&6FFF -- the card is identical, only the connector differs.
    PC1500Machine pc1500a(PC1500Variant::PC1500A);
    pc1500a.attachExpansionCard(bundledCard("ce1638.card.yaml", CardHost::PC1500A));
    pc1500a.memory().writeME0(0x1000, 0xCC);
    pc1500a.memory().writeME0(0x6803, 0x00); // low nibble 0x3 -> bank 3
    pc1500a.memory().writeME0(0x1000, 0xDD);
    CHECK(pc1500a.memory().readME0(0x1000) == 0xDD);
    pc1500a.memory().writeME0(0x6800, 0x00); // back to bank 0
    CHECK(pc1500a.memory().readME0(0x1000) == 0xCC);
}

} // namespace

int run_ce1638_tests() {
    test_ce1638_banking();
    test_ce1638_end_to_end_via_machine();

    std::printf("ce1638_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
