// Headless C++ tests for the CE-1638+ banked-RAM proof of concept
// (Core/Connector/CE1638PlusCard.hpp) and its preset-loader hook
// (PresetFile::memoryExpansionModule == "ce1638plus", applyPC1500Preset() --
// exercised indirectly here via parsePresetFile(), same as
// ce155_tests.cpp). Same no-framework, assert-and-tally style as
// lh5801_tests.cpp/connector_tests.cpp/ce155_tests.cpp.
//
// The card decodes purely from its edge-connector pins (banked window on
// pin 4, bank-latch strobe on pin 18, two unbanked 2KB regions on pins
// 16/17), with no host/variant awareness -- so these tests drive the
// physical pins directly. Which address window a host drives those pins for
// is the connector's job and is covered by connector_tests.cpp.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <string>

#include "../Connector/CE1638PlusCard.hpp"
#include "../PC1500/PC1500Machine.hpp"
#include "../PC1500/PresetFile.hpp"
#include "PresetTestSupport.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

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

void test_ce1638plus_banking() {
    CE1638PlusCard card;
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
    CHECK(card.respondsToRead(makePins(0x0000, /*y0=*/true), v) && v == 0xFF);
    CHECK(card.respondsToWrite(makePins(0x0000, /*y0=*/true), 0x33));
    CHECK(card.respondsToRead(makePins(0x0000, /*y0=*/true), v) && v == 0x33);

    // Switch back to bank 0: bank 0's earlier content (0x11) is intact.
    CHECK(card.respondsToWrite(triggerAt(0x4800), 0x00)); // low nibble 0x0
    CHECK(card.currentBank() == 0);
    CHECK(card.respondsToRead(makePins(0x0000, /*y0=*/true), v) && v == 0x11);

    // A0-A3 can encode 0-15, but only 8 banks exist -- wraps mod 8.
    CHECK(card.respondsToWrite(triggerAt(0x480D), 0x00)); // low nibble 0xD (13) -> bank 5
    CHECK(card.currentBank() == 5);

    // The trigger pin itself is not readable (write-strobe only).
    CHECK(!card.respondsToRead(triggerAt(0x4800), v));
}

void test_ce1638plus_unbanked_regions() {
    CE1638PlusCard card;
    uint8_t v;

    // Two independent 2KB regions on pins 16/17.
    PinState r0 = makePins(0x4800); r0.pin[16] = true;
    PinState r1 = makePins(0x5000); r1.pin[17] = true;
    CHECK(card.respondsToWrite(r0, 0x41));
    CHECK(card.respondsToWrite(r1, 0x42));
    CHECK(card.respondsToRead(r0, v) && v == 0x41);
    CHECK(card.respondsToRead(r1, v) && v == 0x42);

    // Pin 5 is not wired on this card -- a strobe there is not claimed.
    PinState p5 = makePins(0x6000); p5.pin[5] = true;
    CHECK(!card.respondsToRead(p5, v));

    // A bare access with no strobe pin asserted is not claimed either.
    CHECK(!card.respondsToRead(makePins(0x4800), v));
}

void test_ce1638plus_end_to_end_via_machine() {
    PC1500Machine pc1500(PC1500Variant::PC1500);
    pc1500.attachExpansionCard(std::make_unique<CE1638PlusCard>());

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
    pc1500a.attachExpansionCard(std::make_unique<CE1638PlusCard>());
    pc1500a.memory().writeME0(0x1000, 0xCC);
    pc1500a.memory().writeME0(0x6803, 0x00); // low nibble 0x3 -> bank 3
    pc1500a.memory().writeME0(0x1000, 0xDD);
    CHECK(pc1500a.memory().readME0(0x1000) == 0xDD);
    pc1500a.memory().writeME0(0x6800, 0x00); // back to bank 0
    CHECK(pc1500a.memory().readME0(0x1000) == 0xCC);
}

bool parsePresetString(const std::string& yaml, PresetFile* out, std::string* error) {
    return ::parsePresetString(yaml, "/tmp/ce1638plus_tests_scratch.pc1500", out, error);
}

void test_preset_parser_accepts_ce1638plus() {
    PresetFile preset;
    std::string error;
    CHECK(parsePresetString(
        "model: PC-1500\n"
        "firmware: roms/PC-1500_A04.ROM\n"
        "memory-expansion:\n"
        "  - module: ce1638plus\n",
        &preset, &error));
    CHECK(preset.memoryExpansionModule == "ce1638plus");
}

} // namespace

int run_ce1638plus_tests() {
    test_ce1638plus_banking();
    test_ce1638plus_unbanked_regions();
    test_ce1638plus_end_to_end_via_machine();
    test_preset_parser_accepts_ce1638plus();

    std::printf("ce1638plus_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
