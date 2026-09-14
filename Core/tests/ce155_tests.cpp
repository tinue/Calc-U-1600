// Headless C++ tests for the CE-155 memory-adapter proof of concept
// (Core/Connector/CE155Card.hpp) and its preset-loader hook
// (PresetFile::memoryExpansionModule, applyPC1500Preset() -- exercised
// indirectly here via parsePresetFile(), since applyPC1500Preset() itself needs
// a real ROM file and isn't otherwise covered by this Core-only test
// binary). Same no-framework, assert-and-tally style as
// lh5801_tests.cpp/connector_tests.cpp. See ce1638plus_tests.cpp for the
// second memory-expansion module's tests.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <string>

#include "../Connector/CE155Card.hpp"
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

PinState makePins(uint16_t addr, bool y0 = false, bool y2 = false) {
    PinState p;
    p.address = addr;
    p.pin[4] = y0;   // Y0 chip select (&0000-&3FFF)
    p.pin[19] = y2;  // Y2 chip select (&8000-&BFFF)
    return p;
}

// The CE-155 decodes purely from its edge-connector pins: chip 0 on pin 4
// (with A11-A13 = 111), chips 1-3 on pins 16/17/18. It has no variant, so
// these tests drive the physical pins directly. Which address window those
// pins carry a strobe for is the connector's job, not the card's, and is
// covered by connector_tests.cpp.
void test_ce155_pin4_subrange_window() {
    CE155Card card;
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
    CE155Card card;
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
    CE155Card cardA;
    PinState a1 = makePins(0x5800); a1.pin[16] = true;
    PinState a3 = makePins(0x6FFF); a3.pin[18] = true;
    CHECK(cardA.respondsToWrite(a1, 0x51));
    CHECK(cardA.respondsToWrite(a3, 0x53));
    CHECK(cardA.respondsToRead(a1, v) && v == 0x51);   // chip 1, offset 0x800
    CHECK(cardA.respondsToRead(a3, v) && v == 0x53);   // chip 3, offset 0x1FFF
}

void test_ce155_end_to_end_via_machine() {
    PC1500Machine pc1500(PC1500Variant::PC1500);
    pc1500.attachExpansionCard(std::make_unique<CE155Card>());
    pc1500.memory().writeME0(0x4800, 0x77);
    CHECK(pc1500.memory().readME0(0x4800) == 0x77);
    pc1500.memory().writeME0(0x3800, 0x66);
    CHECK(pc1500.memory().readME0(0x3800) == 0x66);

    PC1500Machine pc1500a(PC1500Variant::PC1500A);
    pc1500a.attachExpansionCard(std::make_unique<CE155Card>());
    pc1500a.memory().writeME0(0x5800, 0x88);
    CHECK(pc1500a.memory().readME0(0x5800) == 0x88);
    // On the PC-1500A, &4800 is built-in RAM -- resolve() claims it before
    // the connector, so the card never shadows it.
    pc1500a.memory().writeME0(0x4800, 0x99);
    CHECK(pc1500a.memory().readME0(0x4800) == 0x99); // built-in RAM answers, not the card
}

bool parsePresetString(const std::string& yaml, PresetFile* out, std::string* error) {
    return ::parsePresetString(yaml, "/tmp/ce155_tests_scratch.pc1500", out, error);
}

void test_preset_parser_accepts_ce155() {
    PresetFile preset;
    std::string error;
    CHECK(parsePresetString(
        "model: PC-1500\n"
        "firmware: roms/PC-1500_A04.ROM\n"
        "memory-expansion:\n"
        "  - module: ce155\n",
        &preset, &error));
    CHECK(preset.memoryExpansionModule == "ce155");
}

void test_preset_parser_rejects_other_module() {
    PresetFile preset;
    std::string error;
    CHECK(!parsePresetString(
        "model: PC-1500\n"
        "memory-expansion:\n"
        "  - module: ce163\n",
        &preset, &error));
    CHECK(!error.empty());
}

void test_preset_parser_rejects_extra_field() {
    PresetFile preset;
    std::string error;
    CHECK(!parsePresetString(
        "model: PC-1500\n"
        "memory-expansion:\n"
        "  - address: 0x0000\n"
        "    module: ce155\n",
        &preset, &error));
}

void test_preset_parser_rejects_second_item() {
    PresetFile preset;
    std::string error;
    CHECK(!parsePresetString(
        "model: PC-1500\n"
        "memory-expansion:\n"
        "  - module: ce155\n"
        "  - module: ce155\n",
        &preset, &error));
}

void test_preset_parser_no_memory_expansion_key_is_unaffected() {
    // Regression: a preset that never mentions memory-expansion at all
    // still parses fine and leaves memoryExpansionModule empty.
    PresetFile preset;
    std::string error;
    CHECK(parsePresetString("model: PC-1500A\n", &preset, &error));
    CHECK(preset.memoryExpansionModule.empty());
}

void test_preset_parser_unquotes_single_and_double_quoted_values() {
    // A `type:` step whose own text contains ": " (e.g. typing a BASIC
    // PRINT of a string literal) needs quoting under pc1500preset's own
    // full YAML parser, or its embedded colon would be misread as a second
    // key/value split there -- this loader's first-colon-only split never
    // needed that, but preset files are shared with that upstream loader,
    // so it still has to strip whichever quote style shows up rather than
    // typing the literal quote characters into the emulator (see
    // ce163_bankswrm.pc1500a for a real preset that hit this).
    PresetFile preset;
    std::string error;
    CHECK(parsePresetString(
        "model: PC-1500A\n"
        "keys:\n"
        "  - type: '10 PRINT \"Bank: 7\"'\n"
        "  - type: \"20 PRINT 'Bank: 0'\"\n"
        "  - type: X=0\n",
        &preset, &error));
    CHECK(preset.sections.size() == 1);
    CHECK(preset.sections[0].kind == PresetSection::Kind::Keys);
    CHECK(preset.sections[0].keys.size() == 3);
    CHECK(preset.sections[0].keys[0].text == "10 PRINT \"Bank: 7\"");
    CHECK(preset.sections[0].keys[1].text == "20 PRINT 'Bank: 0'");
    CHECK(preset.sections[0].keys[2].text == "X=0"); // unquoted values still pass through unchanged
}

void test_preset_parser_sequential_keys_and_program_blocks() {
    // The fork from the upstream format (PresetFile.hpp's top-of-file
    // comment): a preset is an ORDERED sequence of `keys:`/`program:`
    // blocks, applied in file order -- not a fixed pre-load-keys/program/
    // post-load-keys triple. Two of each here, interleaved, must come back
    // as four sections in exactly this order.
    PresetFile preset;
    std::string error;
    CHECK(parsePresetString(
        "model: PC-1500A\n"
        "keys:\n"
        "  - type: CALL&4100\n"
        "program:\n"
        "  format: binary\n"
        "  path: memtest_bank.bin\n"
        "  address: 0x7C01\n"
        "keys:\n"
        "  - type: X$=\"3.8\"\n"
        "  - type: CALL&7C01,X$\n",
        &preset, &error));
    CHECK(preset.sections.size() == 3);
    CHECK(preset.sections[0].kind == PresetSection::Kind::Keys);
    CHECK(preset.sections[0].keys.size() == 1);
    CHECK(preset.sections[0].keys[0].text == "CALL&4100");
    CHECK(preset.sections[1].kind == PresetSection::Kind::Program);
    CHECK(preset.sections[1].program.address == 0x7C01);
    CHECK(preset.sections[2].kind == PresetSection::Kind::Keys);
    CHECK(preset.sections[2].keys.size() == 2);
    CHECK(preset.sections[2].keys[0].text == "X$=\"3.8\"");
    CHECK(preset.sections[2].keys[1].text == "CALL&7C01,X$");
}

void test_preset_parser_rejects_old_pre_post_load_keys() {
    // pre-load-keys/post-load-keys are no longer recognized at all -- both
    // are now a single, repeatable 'keys:' block name.
    PresetFile preset;
    std::string error;
    CHECK(!parsePresetString("model: PC-1500A\npre-load-keys:\n  - key: cl\n", &preset, &error));
    CHECK(!parsePresetString("model: PC-1500A\npost-load-keys:\n  - key: cl\n", &preset, &error));
}

void test_preset_parser_firmware_bare_revision() {
    // `firmware: A03` -- the preferred form, naming *which* ROM to run
    // without pretending to know *where* its file lives (a GUI app never
    // wants a preset dictating a raw disk path -- see
    // PresetFile::romVariant's own doc comment).
    PresetFile preset;
    std::string error;
    CHECK(parsePresetString("model: PC-1500\nfirmware: A03\n", &preset, &error));
    CHECK(preset.romVariant == "A03");
}

void test_preset_parser_firmware_path_still_works() {
    // Backward compatibility: existing preset files (this project's own
    // examples/, and pc1500preset's own convention) spell this as a
    // `.../PC-1500_A0N.ROM`-shaped path -- only the "A0N" is extracted,
    // the path itself is never used as a real filesystem path by this
    // parser (see PresetFile::romVariant's own doc comment on why WHERE
    // the ROM file lives is deliberately not this struct's concern).
    PresetFile preset;
    std::string error;
    CHECK(parsePresetString("model: PC-1500\nfirmware: ../roms/PC-1500_A01.ROM\n", &preset, &error));
    CHECK(preset.romVariant == "A01");
}

void test_preset_parser_firmware_ignored_for_pc1500a() {
    // PC-1500A can only run A04 -- unconditional, regardless of whatever
    // (if anything) firmware: says, bare revision or path alike.
    PresetFile preset;
    std::string error;
    CHECK(parsePresetString("model: PC-1500A\nfirmware: A01\n", &preset, &error));
    CHECK(preset.romVariant == "A04");
}

} // namespace

int run_ce155_tests() {
    test_ce155_pin4_subrange_window();
    test_ce155_pins_16_17_18_chips();
    test_ce155_end_to_end_via_machine();
    test_preset_parser_accepts_ce155();
    test_preset_parser_rejects_other_module();
    test_preset_parser_rejects_extra_field();
    test_preset_parser_rejects_second_item();
    test_preset_parser_no_memory_expansion_key_is_unaffected();
    test_preset_parser_unquotes_single_and_double_quoted_values();
    test_preset_parser_sequential_keys_and_program_blocks();
    test_preset_parser_rejects_old_pre_post_load_keys();
    test_preset_parser_firmware_bare_revision();
    test_preset_parser_firmware_path_still_works();
    test_preset_parser_firmware_ignored_for_pc1500a();

    std::printf("ce155_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
