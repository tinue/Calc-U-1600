// Headless C++ tests for the CE-163F 16-bank module (8 RAM + 8 FLASH banks)
// -- Core/Connector/CE163FCard.hpp -- and its preset-loader hook
// (PresetFile::memoryExpansionModule == "ce163f"). Same no-framework,
// assert-and-tally style as ce1638plus_tests.cpp.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <string>

#include "../Connector/CE163FCard.hpp"
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

PinState win(uint16_t addr) { // a plain access into the pin-4 (Y0) bank window
    PinState p;
    p.address = addr;
    p.pin[4] = true;
    return p;
}

PinState trigger(uint16_t lowNibble) { // bank-latch strobe on physical pin 18
    PinState p;
    p.address = uint16_t(0x4800 | (lowNibble & 0x0F));
    p.pin[18] = true;
    return p;
}

// Drive the JEDEC byte-program sequence as guest-CPU writes (direct=false).
void flashProgram(CE163FCard& card, uint16_t addr, uint8_t data) {
    card.respondsToWrite(win(0x1555), 0xAA);
    card.respondsToWrite(win(0x2AAA), 0x55);
    card.respondsToWrite(win(0x1555), 0xA0);
    card.respondsToWrite(win(addr), data);
}

void flashSectorErase(CE163FCard& card, uint16_t sectorAddr) {
    card.respondsToWrite(win(0x1555), 0xAA);
    card.respondsToWrite(win(0x2AAA), 0x55);
    card.respondsToWrite(win(0x1555), 0x80);
    card.respondsToWrite(win(0x1555), 0xAA);
    card.respondsToWrite(win(0x2AAA), 0x55);
    card.respondsToWrite(win(sectorAddr), 0x30);
}

uint8_t readWin(CE163FCard& card, uint16_t addr) {
    uint8_t v = 0;
    card.respondsToRead(win(addr), v);
    return v;
}

void test_ce163f_16_banks_no_mod_wrap() {
    CE163FCard card;

    CHECK(card.currentBank() == 0);

    // Low nibble 0xD latches bank 13 -- NOT bank 5 (no CE1638Plus-style
    // mod-8 wrap; this module has 16 real banks).
    card.respondsToWrite(trigger(0xD), 0x00);
    CHECK(card.currentBank() == 13);

    // Bank 5 (RAM) and bank 13 are distinct 16KB regions.
    card.respondsToWrite(trigger(0x5), 0x00);
    CHECK(card.currentBank() == 5);
    card.respondsToWrite(win(0x0000), 0x22);
    CHECK(readWin(card, 0x0000) == 0x22);
    card.respondsToWrite(trigger(0xD), 0x00);
    CHECK(readWin(card, 0x0000) == 0xAA); // bank 13 (flash) still at its init pattern
}

void test_ce163f_init_pattern_ram_vs_flash() {
    CE163FCard card;
    for (int b = 0; b < 16; b++) {
        card.respondsToWrite(trigger(uint16_t(b)), 0x00);
        uint8_t expected = (b < 8) ? 0xFF : 0xAA; // flash banks power up 0xAA
        CHECK(readWin(card, 0x0000) == expected);
        CHECK(readWin(card, 0x3FFF) == expected);
    }
}

void test_ce163f_ram_banks_freely_writable() {
    CE163FCard card;
    for (int b = 0; b <= 7; b++) {
        card.respondsToWrite(trigger(uint16_t(b)), 0x00);
        CHECK(card.currentBank() == b);
        card.respondsToWrite(win(0x0100), uint8_t(0x40 + b));
        card.respondsToWrite(win(0x3FFF), uint8_t(0x80 + b));
        CHECK(readWin(card, 0x0100) == uint8_t(0x40 + b));
        CHECK(readWin(card, 0x3FFF) == uint8_t(0x80 + b));
    }
}

void test_ce163f_flash_bank_locked_by_default() {
    CE163FCard card;
    card.respondsToWrite(trigger(0x9), 0x00); // bank 9 == flash
    CHECK(card.currentBank() == 9);

    // Flash banks power up with the 0xAA analysis pattern, not 0xFF.
    CHECK(readWin(card, 0x0100) == 0xAA);

    // A bare guest-CPU store does nothing to a flash bank.
    CHECK(card.respondsToWrite(win(0x0100), 0x5A)); // still "claimed"
    CHECK(readWin(card, 0x0100) == 0xAA);
}

void test_ce163f_flash_program_and_and_semantics() {
    CE163FCard card;
    card.respondsToWrite(trigger(0x9), 0x00);

    // Programming can only clear bits, so a byte must be erased (0xFF)
    // before an arbitrary value will take.
    flashSectorErase(card, 0x0100);
    flashProgram(card, 0x0100, 0x5A);
    CHECK(readWin(card, 0x0100) == 0x5A);
    CHECK(card.flashIdle());

    // Re-programming without an erase can only clear more bits (NOR):
    // 0x5A & 0x0F == 0x0A.
    flashProgram(card, 0x0100, 0x0F);
    CHECK(readWin(card, 0x0100) == 0x0A);
}

// A data byte of 0xF0 in the program cycle must be programmed, not eaten as
// the JEDEC software-reset command. The chip only decodes 0xF0->reset while
// awaiting a command; once ProgramArmed (0xA0 seen) the next write is the
// data+address verbatim. Firmware like utilrm.bas's LOADER_HELPER_2 depends
// on this: its `STA (DE) / CPA (DE) / JR NZ` verify poll would spin forever
// if the 0xF0 byte were swallowed as a reset instead of programmed.
void test_ce163f_program_data_byte_0xF0_is_not_a_reset() {
    CE163FCard card;
    card.respondsToWrite(trigger(0x9), 0x00);

    flashSectorErase(card, 0x0100);   // cell -> 0xFF
    flashProgram(card, 0x0100, 0xF0); // program the literal value 0xF0
    CHECK(readWin(card, 0x0100) == 0xF0);
    CHECK(card.flashIdle());

    // 0xF0 written outside a program cycle still resets to read mode: a lone
    // (0x1555,0xAA) half-unlock followed by (x,0xF0) must abort cleanly, and
    // a following full program sequence must still work.
    card.respondsToWrite(win(0x1555), 0xAA);
    card.respondsToWrite(win(0x0000), 0xF0); // reset -> Idle
    CHECK(card.flashIdle());
    flashSectorErase(card, 0x0100);
    flashProgram(card, 0x0100, 0x3C);
    CHECK(readWin(card, 0x0100) == 0x3C);
}

void test_ce163f_low_11_bit_command_decode() {
    CE163FCard card;
    card.respondsToWrite(trigger(0xA), 0x00); // bank 10 == flash
    flashSectorErase(card, 0x0200);

    // Same low 11 bits as &1555/&2AAA, different high bits -- the chip only
    // decodes the low 11, so the sequence must still take.
    card.respondsToWrite(win(0x0555), 0xAA);
    card.respondsToWrite(win(0x2AAA), 0x55);
    card.respondsToWrite(win(0x0555), 0xA0);
    card.respondsToWrite(win(0x0200), 0x3C);
    CHECK(readWin(card, 0x0200) == 0x3C);
}

void test_ce163f_sector_erase() {
    CE163FCard card;
    card.respondsToWrite(trigger(0xB), 0x00); // bank 11 == flash

    flashSectorErase(card, 0x0100); // sector 0 (&0000-&0FFF)
    flashSectorErase(card, 0x2100); // sector 2 (&2000-&2FFF)
    flashProgram(card, 0x0100, 0x11);
    flashProgram(card, 0x2100, 0x22);
    CHECK(readWin(card, 0x0100) == 0x11);
    CHECK(readWin(card, 0x2100) == 0x22);

    flashSectorErase(card, 0x0100);
    CHECK(readWin(card, 0x0100) == 0xFF); // sector 0 wiped
    CHECK(readWin(card, 0x2100) == 0x22); // sector 2 intact
    CHECK(card.flashIdle());
}

void test_ce163f_chip_erase_flash_only() {
    CE163FCard card;

    // Dirty a RAM bank and two flash banks.
    card.respondsToWrite(trigger(0x0), 0x00);
    card.respondsToWrite(win(0x0000), 0xC3); // RAM bank 0
    card.respondsToWrite(trigger(0x8), 0x00);
    flashSectorErase(card, 0x0000);
    flashProgram(card, 0x0000, 0x81); // flash bank 8
    card.respondsToWrite(trigger(0xF), 0x00);
    flashSectorErase(card, 0x0000);
    flashProgram(card, 0x0000, 0x8F); // flash bank 15
    CHECK(readWin(card, 0x0000) == 0x8F);

    // Chip erase from flash bank 15.
    card.respondsToWrite(win(0x1555), 0xAA);
    card.respondsToWrite(win(0x2AAA), 0x55);
    card.respondsToWrite(win(0x1555), 0x80);
    card.respondsToWrite(win(0x1555), 0xAA);
    card.respondsToWrite(win(0x2AAA), 0x55);
    card.respondsToWrite(win(0x1555), 0x10); // chip-erase confirm goes to the command address

    CHECK(readWin(card, 0x0000) == 0xFF);           // bank 15 erased
    card.respondsToWrite(trigger(0x8), 0x00);
    CHECK(readWin(card, 0x0000) == 0xFF);           // bank 8 erased
    card.respondsToWrite(trigger(0x0), 0x00);
    CHECK(readWin(card, 0x0000) == 0xC3);           // RAM bank 0 untouched
}

void test_ce163f_broken_sequence_resets() {
    CE163FCard card;
    card.respondsToWrite(trigger(0x9), 0x00);

    // Stray write mid-sequence aborts it cleanly.
    card.respondsToWrite(win(0x1555), 0xAA);
    card.respondsToWrite(win(0x1555), 0x00); // not (0x2AA,0x55) -> back to Idle
    CHECK(card.flashIdle());
    card.respondsToWrite(win(0x0100), 0x42); // bare write -- inert
    CHECK(readWin(card, 0x0100) == 0xAA);

    // A wrong middle cycle leaves memory unchanged...
    card.respondsToWrite(win(0x1555), 0xAA);
    card.respondsToWrite(win(0x2AAA), 0x54); // wrong data
    card.respondsToWrite(win(0x1555), 0xA0);
    card.respondsToWrite(win(0x0100), 0x42);
    CHECK(readWin(card, 0x0100) == 0xAA);

    // ...and a clean erase + program right after still works.
    flashSectorErase(card, 0x0100);
    flashProgram(card, 0x0100, 0x42);
    CHECK(readWin(card, 0x0100) == 0x42);
}

void test_ce163f_trigger_is_pin_18() {
    // The bank latch is physical pin 18 only -- whatever the host calls it
    // (S3 on a PC-1500, S5 on a PC-1500A). A strobe on any other pin does
    // not latch a bank.
    CE163FCard card;
    PinState t;
    t.address = 0x6803;
    t.pin[18] = true;
    card.respondsToWrite(t, 0x00);
    CHECK(card.currentBank() == 3);

    PinState notTrigger;
    notTrigger.address = 0x5807;
    notTrigger.pin[16] = true; // a different pin -- must not move the bank
    card.respondsToWrite(notTrigger, 0x00);
    CHECK(card.currentBank() == 3); // unchanged
}

void test_ce163f_end_to_end_via_machine_and_poke_bypass() {
    PC1500Machine pc1500(PC1500Variant::PC1500);
    pc1500.attachExpansionCard(std::make_unique<CE163FCard>());

    // RAM bank 2 via guest-CPU writes.
    pc1500.memory().writeME0(0x5802, 0x00); // strobe: S3, low nibble 2
    pc1500.memory().writeME0(0x0100, 0xAA);
    CHECK(pc1500.memory().readME0(0x0100) == 0xAA);

    // Flash bank 9: powers up 0xAA, and a guest-CPU store is inert without
    // an unlock...
    pc1500.memory().writeME0(0x5809, 0x00); // strobe: low nibble 9
    CHECK(pc1500.memory().readME0(0x0101) == 0xAA);
    pc1500.memory().writeME0(0x0101, 0x43);
    CHECK(pc1500.memory().readME0(0x0101) == 0xAA);

    // ...but poke() (the preset-loader / debug path) writes it directly
    // (sector 3, clear of the JEDEC erase below).
    pc1500.memory().poke(0x3000, 0x42);
    CHECK(pc1500.memory().readME0(0x3000) == 0x42);

    // The full erase + program flow also works end-to-end through the CPU bus.
    pc1500.memory().writeME0(0x1555, 0xAA);
    pc1500.memory().writeME0(0x2AAA, 0x55);
    pc1500.memory().writeME0(0x1555, 0x80);
    pc1500.memory().writeME0(0x1555, 0xAA);
    pc1500.memory().writeME0(0x2AAA, 0x55);
    pc1500.memory().writeME0(0x0000, 0x30); // erase sector 0 of bank 9
    CHECK(pc1500.memory().readME0(0x0102) == 0xFF);
    pc1500.memory().writeME0(0x1555, 0xAA);
    pc1500.memory().writeME0(0x2AAA, 0x55);
    pc1500.memory().writeME0(0x1555, 0xA0);
    pc1500.memory().writeME0(0x0102, 0x77);
    CHECK(pc1500.memory().readME0(0x0102) == 0x77);
}

// The real CE163F firmware's unlock cycle interleaves writes to the
// module's bank/command ports (&6808/&6809 on PC-1500A -- both in the S5
// strobe range) with the flash-address writes (&1555/&2AAA), e.g.
// updaterm-selfpatch.asm LOADER_HELPER_3:
//   STA(&6809,AA) STA(&1555,AA) STA(&6808,55) STA(&2AAA,55)
// The bank strobe must NOT reset the chip's command decoder, or the unlock
// never completes, the flash never arms, and the firmware's post-program
// `CPA (DE) / JR NZ` verify loop spins forever.
void test_ce163f_interleaved_bank_strobes_during_unlock() {
    PC1500Machine pc(PC1500Variant::PC1500A);
    pc.attachExpansionCard(std::make_unique<CE163FCard>());
    auto& m = pc.memory();
    auto unlock = [&] {                       // LOADER_HELPER_3
        m.writeME0(0x6809, 0xAA); m.writeME0(0x1555, 0xAA);
        m.writeME0(0x6808, 0x55); m.writeME0(0x2AAA, 0x55);
    };

    m.writeME0(0x6808, 0x08); // select flash bank 8

    // Erase sector 0 (LOADER_HELPER_1).
    unlock();
    m.writeME0(0x6809, 0x80); m.writeME0(0x1555, 0x80);
    unlock();
    m.writeME0(0x6808, 0x55);
    m.writeME0(0x0000, 0x30);
    CHECK(m.readME0(0x0000) == 0xFF); // sector erased (was 0xAA init fill)

    // Program a byte (LOADER_HELPER_2).
    m.writeME0(0x6808, 0x08); // re-select bank 8
    unlock();
    m.writeME0(0x6809, 0xA0); m.writeME0(0x1555, 0xA0);
    m.writeME0(0x6808, 0xA0);
    m.writeME0(0x0000, 0x48);
    CHECK(m.readME0(0x0000) == 0x48); // byte took -> verify loop would exit
}

bool parsePresetString(const std::string& yaml, PresetFile* out, std::string* error) {
    return ::parsePresetString(yaml, "/tmp/ce163f_tests_scratch.pc1500", out, error);
}

void test_preset_parser_accepts_ce163f() {
    PresetFile preset;
    std::string error;
    CHECK(parsePresetString(
        "model: PC-1500A\n"
        "firmware: roms/PC-1500_A04.ROM\n"
        "memory-expansion:\n"
        "  - module: ce163f\n",
        &preset, &error));
    CHECK(preset.memoryExpansionModule == "ce163f");
}

} // namespace

int run_ce163f_tests() {
    test_ce163f_16_banks_no_mod_wrap();
    test_ce163f_init_pattern_ram_vs_flash();
    test_ce163f_ram_banks_freely_writable();
    test_ce163f_flash_bank_locked_by_default();
    test_ce163f_flash_program_and_and_semantics();
    test_ce163f_program_data_byte_0xF0_is_not_a_reset();
    test_ce163f_low_11_bit_command_decode();
    test_ce163f_sector_erase();
    test_ce163f_chip_erase_flash_only();
    test_ce163f_broken_sequence_resets();
    test_ce163f_trigger_is_pin_18();
    test_ce163f_interleaved_bank_strobes_during_unlock();
    test_ce163f_end_to_end_via_machine_and_poke_bypass();
    test_preset_parser_accepts_ce163f();

    std::printf("ce163f_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
