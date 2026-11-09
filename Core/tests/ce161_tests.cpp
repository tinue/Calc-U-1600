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

#include "../PC1500/PC1500BasicTyper.hpp"
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

// The Instruction Manual's "exclusive readout" procedure against the real
// A04 ROM (docs/background/CE-161-Program-Module.md): NEW 256 puts BASIC at
// &0100, the program ends in FF padding plus `65279 END` at &3FF9, the
// header POKE marks &0000 as a module. With the switch on, the power-on
// scan (RESET_9, E098) finds page &00 not writable and takes the header:
// ST_ROM_MOD = 00, program start &0100, RAM from &4000; NEW 0 then starts
// RAM BASIC at &4000, and the module program still runs by its label.
// The PC-1500 ends RAM at &4800 (18 KB in all, the manual's figure); the
// PC-1500A at &5800.
void protectedProgramModule(PC1500Variant variant) {
    const bool is1500A = variant == PC1500Variant::PC1500A;
    const uint8_t ramEnd = is1500A ? 0x58 : 0x48;
    CE161_OR_FAIL(card, is1500A ? CardHost::PC1500A : CardHost::PC1500);
    PC1500Machine m(variant);
    m.attachExpansionCard(std::move(card));
    if (!m.loadROMFile("roms/PC-1500_A04.ROM")) {
        std::fprintf(stderr, "SKIP %s: roms/PC-1500_A04.ROM not found (run from the repo root)\n", __func__);
        return;
    }
    m.reset();
    runBootToPrompt(m);
    auto peek16 = [&](uint16_t a) { return uint16_t(m.memory().peek(a) << 8 | m.memory().peek(a + 1)); };

    tapKey(m, "cl");
    CHECK(typeLine(m, "NEW 256", true, nullptr));
    CHECK(m.memory().peek(0x7863) == 0x00 && m.memory().peek(0x7864) == ramEnd);
    CHECK(peek16(0x7865) == 0x0100);
    CHECK(typeBasicProgramText(m, "10 \"A\":POKE &7C50,65:END\n").ok);

    // The manual's fill program, done from the harness: FF from the
    // program's end marker, then `65279 END` + end marker ending at &3FFF.
    const uint16_t end = peek16(0x7867);
    CHECK(end > 0x0100 && end < 0x3FF9);
    for (uint32_t a = end; a < 0x3FF9; a++) m.memory().writeME0(uint16_t(a), 0xFF);
    const uint8_t endLine[] = {0xFE, 0xFF, 0x03, 0xF1, 0x8E, 0x0D, 0xFF};
    for (int i = 0; i < 7; i++) m.memory().writeME0(uint16_t(0x3FF9 + i), endLine[i]);
    CHECK(typeLine(m, "POKE 0,85,0,1,0,0,0,0,255", true, nullptr));
    CHECK(m.memory().peek(0x0000) == 0x55 && m.memory().peek(0x0007) == 0xFF);

    // Flip the switch, OFF/ON.
    ExpansionCard* c = m.expansionConnector().attachedCard();
    CHECK(c && c->hasWriteProtectSwitch());
    if (!c) return;
    c->setWriteProtected(true);
    tapKey(m, "off");
    m.runCycles(static_cast<uint64_t>(PC1500Machine::kCpuHz / 2));
    m.setOnKeyPressed(true);
    m.runCycles(static_cast<uint64_t>(PC1500Machine::kCpuHz / 10));
    m.setOnKeyPressed(false);
    runBootToPrompt(m);
    CHECK(m.memory().peek(0x7860) == 0x00);  // ST_ROM_MOD: module at page 00
    CHECK(peek16(0x7861) == 0x0100);         // its program start
    CHECK(m.memory().peek(0x7863) == 0x40 && m.memory().peek(0x7864) == ramEnd);  // RAM = built-in only

    tapKey(m, "cl");
    CHECK(typeLine(m, "NEW 0", true, nullptr));
    CHECK(peek16(0x7865) == 0x4000);  // RAM BASIC right after the module
    m.memory().writeME0(0x7C50, 0x00);
    tapKey(m, "mode");  // PRO -> RUN
    CHECK(typeLine(m, "GOTO \"A\"", true, nullptr));
    CHECK(m.memory().peek(0x7C50) == 65);  // the module's program ran
    CHECK(typeLine(m, "POKE &100,0", true, nullptr));
    CHECK(m.memory().peek(0x0100) == 0x00 && m.memory().peek(0x0101) == 0x0A);  // line 10 untouched
}

void test_ce161_protected_program_module_with_rom() {
    protectedProgramModule(PC1500Variant::PC1500);
    protectedProgramModule(PC1500Variant::PC1500A);
}

} // namespace

int run_ce161_tests() {
    test_ce161_pin4_full_window();
    test_ce161_end_to_end_via_machine();
    test_ce161_hosts();
    test_ce161_protected_program_module_with_rom();

    std::printf("ce161_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
