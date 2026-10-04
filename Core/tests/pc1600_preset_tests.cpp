// Headless C++ tests for the `model: PC-1600` preset path: the parser
// extension in PresetFile.cpp (slot-1:/slot-2:, PC-1600 model)
// and Core/PC1600/PC1600PresetLoader.cpp. Same no-framework, assert-and-
// tally style as lh5801_tests.cpp.
//
// Build & run: see tools/run_tests.sh


#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <unistd.h> // mkdtemp

#include "../Connector/CE1600FCard.hpp"
#include "../Connector/FloppyImageFile.hpp"
#include "../PC1600/PC1600Machine.hpp"
#include "../PC1600/PC1600PresetLoader.hpp"
#include "../PC1600/PC1600PresetMedia.hpp"
#include "PresetTestSupport.hpp"
#include "TestRoms.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

bool parse(const std::string& yaml, PresetFile* out, std::string* error) {
    return parsePresetString(yaml, "/tmp/pc1600_preset_tests_scratch.pc1600", out, error);
}

void test_parser_accepts_pc1600_with_slot_and_keys() {
    PresetFile p;
    std::string err;
    CHECK(parse(
        "model: PC-1600\n"
        "slot-1: CE-155\n"
        "keys:\n"
        "  - type: MEM\n"
        "  - key: enter\n",
        &p, &err));
    CHECK(p.isPC1600());
    CHECK(p.slot1ModuleSpecName == "CE-155");
    CHECK(p.slot2ModuleSpecName.empty() && p.slot2ModuleSpecFile.empty());
    CHECK(p.sections.size() == 1);
    CHECK(p.sections[0].kind == PresetSection::Kind::Keys);
    CHECK(p.sections[0].keys.size() == 2);
    CHECK(p.sections[0].keys[0].kind == PresetStep::Kind::Type);
    CHECK(p.sections[0].keys[0].text == "MEM");
    CHECK(p.sections[0].keys[1].kind == PresetStep::Kind::Key);
}

void test_parser_rejects_multichar_key() {
    // `key: MEM` is an error (it would press nothing) -- must use `type:`.
    // Same rule as the PC-1500 side.
    PresetFile p;
    std::string err;
    CHECK(!parse("model: PC-1600\nkeys:\n  - key: MEM\n", &p, &err));
    CHECK(err.find("MEM") != std::string::npos);
}

void test_parser_both_slots() {
    PresetFile p;
    std::string err;
    CHECK(parse(
        "model: PC-1600\n"
        "slot-1: CE-1600M\n"
        "slot-2: CE-1601M\n",
        &p, &err));
    CHECK(p.slot1ModuleSpecName == "CE-1600M");
    CHECK(p.slot2ModuleSpecName == "CE-1601M");
}

void test_parser_rejects_cross_model_fields() {
    PresetFile p;
    std::string err;
    // PC-1600 preset with a firmware: field (gone -- the ROM rides on the model).
    CHECK(!parse("model: PC-1600\nfirmware: A04\n", &p, &err));
    CHECK(!err.empty());
    // A memory-expansion: block is refused.
    CHECK(!parse("model: PC-1600\nmemory-expansion:\n  - modulespec: CE-155\n", &p, &err));
    // PC-1600 machine code needs no target slot: the loader
    // follows MODE / TITLE (see test_parser_pc1600_machine_binary).
    p = PresetFile{};
    CHECK(parse("model: PC-1600\nprogram:\n  file: x.bin\n  address: 0x8000\n", &p, &err));
    // PC-1500 preset with the PC-1600's second slot.
    p = PresetFile{};
    CHECK(!parse("model: PC-1500A\nslot-2: CE-155\n", &p, &err));
}

void test_parser_pc1600_machine_binary() {
    // parsePresetFile() merges into *out instead of resetting it, so each
    // case that inspects the parse result gets its own PresetFile.
    std::string err;

    // `address` / `length` optional (a header may supply them).
    {
        PresetFile p;
        CHECK(parse("model: PC-1600\nprogram:\n  file: x.bin\n", &p, &err));
        CHECK(p.sections.size() == 1);
        CHECK(p.sections[0].program.format == PresetProgram::Format::Binary);
        CHECK(!p.sections[0].program.hasAddress);
        CHECK(!p.sections[0].program.hasLength);
    }

    // `address` / `length` overrides, both bases.
    {
        PresetFile p;
        CHECK(parse("model: PC-1600\nprogram:\n  file: x.bin\n"
                    "  address: 0x8100\n  length: 0x40\n",
                    &p, &err));
        CHECK(p.sections[0].program.hasAddress && p.sections[0].program.address == 0x8100);
        CHECK(p.sections[0].program.hasLength && p.sections[0].program.length == 0x40);
    }
    {
        PresetFile p;
        CHECK(parse("model: PC-1600\nprogram:\n  file: x.bin\n  length: 256\n", &p, &err));
        CHECK(p.sections[0].program.length == 256);
    }

    // `slot:` is refused (the loader follows MODE / TITLE) -- on every
    // model and format; bad length; length on the wrong format.
    for (const char* preset : {"model: PC-1600\nprogram:\n  slot: S0\n  file: x.bin\n",
                               "model: PC-1600\nprogram:\n  slot: S0\n  file: x.bas\n",
                               "model: PC-1500A\nprogram:\n  slot: S0\n  file: x.bin\n"}) {
        PresetFile p;
        CHECK(!parse(preset, &p, &err));
        CHECK(err.find("'slot:' was removed") != std::string::npos);
    }
    {
        PresetFile p;
        CHECK(!parse("model: PC-1600\nprogram:\n  file: x.bin\n  length: 12x\n", &p, &err));
    }
    {
        PresetFile p;
        CHECK(!parse("model: PC-1600\nprogram:\n  length: 8\n  file: x.bas\n", &p, &err));
        CHECK(err.find("length") != std::string::npos);
    }

    // PC-1500 machine code takes `address` / `length` like the PC-1600:
    // both optional at parse time (a CE-158 header can supply them; the
    // loader refuses a headerless file without `address`).
    {
        PresetFile p;
        CHECK(parse("model: PC-1500A\nprogram:\n  file: x.bin\n", &p, &err));
        CHECK(!p.sections[0].program.hasAddress);
    }
    {
        PresetFile p;
        CHECK(parse("model: PC-1500A\nprogram:\n  file: x.bin\n  address: 0x40C5\n"
                    "  length: 8\n",
                    &p, &err));
        CHECK(p.sections[0].program.hasLength && p.sections[0].program.length == 8);
    }
}

// A 16-byte PC-1600 machine-language transfer header + payload.
std::vector<uint8_t> mlFile(const std::vector<uint8_t>& payload, uint32_t loadAddr,
                            uint32_t autorunAddr, uint32_t declaredLen) {
    std::vector<uint8_t> f(16, 0x00);
    f[0] = 0xFF; f[1] = 0x10; f[2] = 0x00; f[3] = 0x00;
    f[4] = 0x10;  // MACHINE
    f[5] = static_cast<uint8_t>(declaredLen & 0xFF);
    f[6] = static_cast<uint8_t>((declaredLen >> 8) & 0xFF);
    f[7] = static_cast<uint8_t>((declaredLen >> 16) & 0xFF);
    f[8] = static_cast<uint8_t>(loadAddr & 0xFF);
    f[9] = static_cast<uint8_t>((loadAddr >> 8) & 0xFF);
    f[10] = static_cast<uint8_t>((loadAddr >> 16) & 0xFF);
    f[11] = static_cast<uint8_t>(autorunAddr & 0xFF);
    f[12] = static_cast<uint8_t>((autorunAddr >> 8) & 0xFF);
    f[13] = static_cast<uint8_t>((autorunAddr >> 16) & 0xFF);
    f[0x0E] = 0x00; f[0x0F] = 0x0F;
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

bool writeFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

bool writeTextFile(const std::string& path, const std::string& text) {
    return writeFile(path, std::vector<uint8_t>(text.begin(), text.end()));
}

// Functional: a machine-code `program:` block with a PC-1600 ML header
// pokes its payload linearly into S0 (internal RAM) at the header's load
// address.
void test_loader_machine_binary_header() {
    PC1600Machine m;
    if (!loadPC1600Roms(m)) {
        std::fprintf(stderr, "SKIP test_loader_machine_binary_header: PC-1600 ROM images not found\n");
        return;
    }
    const std::string bin = "/tmp/pc1600_ml_header.bin";
    CHECK(writeFile(bin, mlFile({0xC9, 0x3E, 0xC9}, /*load=*/0xD000, /*autorun=*/0, /*declared=*/3)));

    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nprogram:\n  file: " + bin + "\n",
                &p, &err));
    PresetLoadResult r = applyPC1600Preset(m, p);
    CHECK(r.ok);
    CHECK(r.error.empty());
    CHECK(m.debugPeek(0xD000) == 0xC9);
    CHECK(m.debugPeek(0xD001) == 0x3E);
    CHECK(m.debugPeek(0xD002) == 0xC9);
}

// Functional: header length field that disagrees with the file is a hard
// error naming `length:`; supplying `length:` overrides it.
void test_loader_machine_binary_length_mismatch() {
    PC1600Machine m;
    if (!loadPC1600Roms(m)) {
        std::fprintf(stderr, "SKIP test_loader_machine_binary_length_mismatch: PC-1600 ROM images not found\n");
        return;
    }
    // Header claims 1 payload byte; the file actually carries 4.
    const std::string bin = "/tmp/pc1600_ml_mismatch.bin";
    CHECK(writeFile(bin, mlFile({0x11, 0x22, 0x33, 0x44}, 0xD100, 0, /*declared=*/1)));

    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nprogram:\n  file: " + bin + "\n",
                &p, &err));
    PresetLoadResult r = applyPC1600Preset(m, p);
    CHECK(!r.ok);
    CHECK(r.error.find("length") != std::string::npos);

    // Same file, explicit length: 4 -> loads all four bytes.
    PC1600Machine m2;
    loadPC1600Roms(m2);
    PresetFile p2;
    CHECK(parse("model: PC-1600\nprogram:\n  file: " + bin +
                    "\n  length: 4\n",
                &p2, &err));
    PresetLoadResult r2 = applyPC1600Preset(m2, p2);
    CHECK(r2.ok);
    CHECK(m2.debugPeek(0xD100) == 0x11);
    CHECK(m2.debugPeek(0xD103) == 0x44);
}

// Functional: a headerless blob needs both `address:` and `length:`.
void test_loader_machine_binary_headerless() {
    PC1600Machine m;
    if (!loadPC1600Roms(m)) {
        std::fprintf(stderr, "SKIP test_loader_machine_binary_headerless: PC-1600 ROM images not found\n");
        return;
    }
    const std::string bin = "/tmp/pc1600_ml_raw.bin";
    CHECK(writeFile(bin, {0xAA, 0xBB, 0xCC, 0xDD}));

    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nprogram:\n  file: " + bin + "\n",
                &p, &err));
    PresetLoadResult r = applyPC1600Preset(m, p);
    CHECK(!r.ok);
    CHECK(r.error.find("required") != std::string::npos);

    PC1600Machine m2;
    loadPC1600Roms(m2);
    PresetFile p2;
    CHECK(parse("model: PC-1600\nprogram:\n  file: " + bin +
                    "\n  address: 0xD200\n  length: 4\n",
                &p2, &err));
    PresetLoadResult r2 = applyPC1600Preset(m2, p2);
    CHECK(r2.ok);
    CHECK(m2.debugPeek(0xD200) == 0xAA);
    CHECK(m2.debugPeek(0xD203) == 0xDD);
}

// Functional: a headerless blob with only `address:` loads the whole file;
// a CE-158 (PC-1500) file is refused rather than poked header and all.
void test_loader_machine_binary_headerless_address_only_and_ce158() {
    PC1600Machine m;
    if (!loadPC1600Roms(m)) {
        std::fprintf(stderr, "SKIP test_loader_machine_binary_headerless_address_only_and_ce158: "
                             "PC-1600 ROM images not found\n");
        return;
    }
    const std::string bin = "/tmp/pc1600_ml_raw_addr.bin";
    CHECK(writeFile(bin, {0x12, 0x34, 0x56}));
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nprogram:\n  file: " + bin +
                    "\n  address: 0xD400\n",
                &p, &err));
    PresetLoadResult r = applyPC1600Preset(m, p);
    CHECK(r.ok);
    CHECK(m.debugPeek(0xD400) == 0x12);
    CHECK(m.debugPeek(0xD402) == 0x56);

    std::vector<uint8_t> ce158 = {0x01, 0x42, 'C', 'O', 'M'};
    ce158.resize(5 + 16, 0);
    for (uint16_t v : {uint16_t{0x40C5}, uint16_t{0}, uint16_t{0}}) {
        ce158.push_back(static_cast<uint8_t>(v >> 8));
        ce158.push_back(static_cast<uint8_t>(v & 0xFF));
    }
    ce158.push_back(0x9A);
    const std::string ce158Bin = "/tmp/pc1600_ml_ce158.bin";
    CHECK(writeFile(ce158Bin, ce158));
    PC1600Machine m2;
    loadPC1600Roms(m2);
    PresetFile p2;
    CHECK(parse("model: PC-1600\nprogram:\n  file: " + ce158Bin +
                    "\n  address: 0xD500\n",
                &p2, &err));
    PresetLoadResult r2 = applyPC1600Preset(m2, p2);
    CHECK(!r2.ok);  // MODE 0: a PC-1500 file needs MODE 1
    CHECK(r2.error.find("CE-158") != std::string::npos);
}

// Functional: a non-zero header auto-run address makes the loader type
// `CALL &<addr>`; a bare RET payload returns immediately and the load
// completes cleanly.
void test_loader_machine_binary_autorun() {
    PC1600Machine m;
    if (!loadPC1600Roms(m)) {
        std::fprintf(stderr, "SKIP test_loader_machine_binary_autorun: PC-1600 ROM images not found\n");
        return;
    }
    const std::string bin = "/tmp/pc1600_ml_autorun.bin";
    CHECK(writeFile(bin, mlFile({0xC9}, /*load=*/0xD300, /*autorun=*/0xD300, /*declared=*/1)));

    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nprogram:\n  file: " + bin + "\n",
                &p, &err));
    PresetLoadResult r = applyPC1600Preset(m, p);
    CHECK(r.ok);
    CHECK(r.error.empty());
    CHECK(m.debugPeek(0xD300) == 0xC9);
}

// ROM-gated: a header auto-run into slot 2 must CALL through global bank 2
// (`CALL #2,&8100`), not `CALL &8100` -- that would run whatever bank 0
// maps there. The code stores a marker in internal RAM, so it only lands
// if the right CALL ran it.
void test_loader_machine_binary_autorun_slot2() {
    PC1600Machine m;
    if (!loadPC1600Roms(m)) {
        std::fprintf(stderr, "SKIP test_loader_machine_binary_autorun_slot2: PC-1600 ROM images not found\n");
        return;
    }
    const std::string bin = "/tmp/pc1600_ml_autorun_s2.bin";
    // LD A,5AH / LD (D300H),A / RET
    CHECK(writeFile(bin, mlFile({0x3E, 0x5A, 0x32, 0x00, 0xD3, 0xC9}, /*load=*/0x8100, /*autorun=*/0x8100,
                                /*declared=*/6)));

    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nslot-2: CE-1600M\n"
                "program:\n  file: " + bin + "\n",
                &p, &err));
    std::string logText;
    PresetLoadResult r = applyPC1600Preset(m, p, [&](const std::string& line) { logText += line + "\n"; }, ".",
                                           "Qt6/resources/cards");
    CHECK(r.ok);
    CHECK(logText.find("auto-run CALL #2,&8100") != std::string::npos);
    CHECK(m.debugPeek(0xD300) == 0x5A);
}

// ROM-gated: the target follows MODE and TITLE
// (docs/background/plans/Loader-Mode-Plan.md).
// MODE 1: a headerless file's `address:` is an LH5803 address (&5000 =
// Z-80 &D000), and a CE-158 file loads at its header's LH5803 address.
void test_loader_machine_binary_mode1() {
    PC1600Machine m;
    if (!loadPC1600Roms(m)) {
        std::fprintf(stderr, "SKIP test_loader_machine_binary_mode1: PC-1600 ROM images not found\n");
        return;
    }
    const std::string bin = "/tmp/pc1600_ml_mode1.bin";
    CHECK(writeFile(bin, {0x5A, 0xA5}));
    std::vector<uint8_t> ce158 = {0x01, 0x42, 'C', 'O', 'M'};
    ce158.resize(5 + 16, 0);
    for (uint16_t v : {uint16_t{0x5100}, uint16_t{0}, uint16_t{0}}) {
        ce158.push_back(static_cast<uint8_t>(v >> 8));
        ce158.push_back(static_cast<uint8_t>(v & 0xFF));
    }
    ce158.push_back(0x9A);
    const std::string ce158Bin = "/tmp/pc1600_ml_mode1_ce158.bin";
    CHECK(writeFile(ce158Bin, ce158));
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nkeys:\n  - type: MODE1\nprogram:\n  file: " + bin +
                    "\n  address: 0x5000\nprogram:\n  file: " + ce158Bin + "\n",
                &p, &err));
    PresetLoadResult r = applyPC1600Preset(m, p);
    CHECK(r.ok);
    if (!r.ok) std::fprintf(stderr, "  %s\n", r.error.c_str());
    CHECK(m.debugPeek(0xD000) == 0x5A && m.debugPeek(0xD001) == 0xA5);
    CHECK(m.debugPeek(0xD100) == 0x9A);
}

// TITLE "S1:": $8000-$BFFF is the S1 program module.
void test_loader_machine_binary_title_s1() {
    PC1600Machine m;
    if (!loadPC1600Roms(m)) {
        std::fprintf(stderr, "SKIP test_loader_machine_binary_title_s1: PC-1600 ROM images not found\n");
        return;
    }
    const std::string bin = "/tmp/pc1600_ml_title_s1.bin";
    CHECK(writeFile(bin, {0x11, 0x22, 0x33}));
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nslot-1: CE-1600M\n"
                "keys:\n  - type: INIT\"S1:\",\"P\"\n  - type: TITLE\"S1:\"\n"
                "program:\n  file: " + bin + "\n  address: 0x8100\n",
                &p, &err));
    PresetLoadResult r = applyPC1600Preset(m, p, nullptr, ".", "Qt6/resources/cards");
    CHECK(r.ok);
    if (!r.ok) std::fprintf(stderr, "  %s\n", r.error.c_str());
    const std::vector<uint8_t> img = m.debugSlotImage(1);
    CHECK(img.size() > 0x102 && img[0x100] == 0x11 && img[0x102] == 0x33);
}

void test_parser_accepts_basic_text_program() {
    PresetFile p;
    std::string err;
    CHECK(parse(
        "model: PC-1600\n"
        "keys:\n"
        "  - key: mode\n"
        "program:\n"
        "  text: |\n"
        "    10 PRINT 1\n"
        "    20 GOTO 10\n",
        &p, &err));
    CHECK(p.sections.size() == 2);
    CHECK(p.sections[0].kind == PresetSection::Kind::Keys);
    CHECK(p.sections[1].kind == PresetSection::Kind::Program);
    CHECK(p.sections[1].program.format == PresetProgram::Format::BasicText);
    CHECK(p.sections[1].program.text.find("20 GOTO 10") != std::string::npos);
}

// ROM-gated: the loader's line-length guard -- in PRO mode a fitting
// program loads and only the over-length line is reported.
void test_loader_reports_overlong_basic_line() {
    PC1600Machine m;
    if (!loadPC1600Roms(m)) {
        std::fprintf(stderr, "SKIP test_loader_reports_overlong_basic_line: PC-1600 ROM images not found\n");
        return;
    }
    std::string longLine = "20 REM ";
    while (longLine.size() < 100) longLine += 'X';

    PresetFile p;
    std::string err;
    CHECK(parse(
        "model: PC-1600\n"
        "keys:\n"
        "  - key: mode\n"
        "program:\n"
        "  text: |\n"
        "    10 PRINT 1\n"
        "    " + longLine + "\n"
        "    30 END\n",
        &p, &err));

    PresetLoadResult r = applyPC1600Preset(m, p);
    CHECK(!r.ok);
    CHECK(r.rejectedBasicLines.size() == 1);
    CHECK(!r.rejectedBasicLines.empty() && r.rejectedBasicLines[0] == longLine);
    CHECK(!r.error.empty());
}

void test_loader_applies_ce155_and_type_step() {
    PresetFile p;
    std::string err;
    CHECK(parse(
        "model: PC-1600\n"
        "slot-1: CE-155\n"
        "keys:\n"
        "  - type: MEM\n",
        &p, &err));

    PC1600Machine m;
    loadPC1600Roms(m);  // optional: the loader boots an empty bus too, only slower
    PresetLoadResult r = applyPC1600Preset(m, p, {}, ".", "Qt6/resources/cards");
    CHECK(r.ok);
    CHECK(m.slot1Attached());
    CHECK(!m.slot2Attached());
    CHECK(!m.ce1600pAttached() && !m.ce150Attached());  // no plotter: key, no plotter

    // The card is live: with page-C bank 0 selected it answers across
    // &A000-&BFFF; &A800 is its pin-17 (S2) block.
    m.memory().writeIO(0x31, 0x00);
    m.memory().write(0xA800, 0x5A);
    CHECK(m.memory().read(0xA800) == 0x5A);
}

void test_type_step_rejects_untypeable_char() {
    PresetFile p;
    std::string err;
    // A control byte (0x01) has no PC-1600 key.
    CHECK(parse(std::string("model: PC-1600\nkeys:\n  - type: a\x01""b\n"), &p, &err));
    PC1600Machine m;
    loadPC1600Roms(m);  // optional: an empty bus boots too, only slower
    PresetLoadResult r = applyPC1600Preset(m, p);
    CHECK(!r.ok);
    CHECK(!r.error.empty());
}

// Functional: with the real ROM booting, a `type:` line with shifted
// punctuation lands the right ASCII in the console input buffer
// (FBB0H-FBFFH, Ref/PC-1600/PC-1600-Work-Area-Map.md) -- i.e. SHIFT + base key really
// produces the character, not a shift that leaks onto the next key
// (which would turn `INIT"S2:","M"` into `INITs2M`).
void test_type_step_shifted_punctuation_reaches_input_buffer() {
    PC1600Machine m;
    if (!loadPC1600Roms(m)) {
        std::fprintf(stderr,
                     "SKIP test_type_step_shifted_punctuation_reaches_input_buffer: PC-1600 ROM images not found\n");
        return;
    }
    PresetFile p;
    std::string err;
    // No ENTER echo to worry about: the step appends ENTER, but we read the
    // buffer right after typing, before the line is consumed. Use a line
    // that never runs anything: just the characters.
    CHECK(parse("model: PC-1600\n"
                "keys:\n"
                "  - type: A\"B:C,D\n",
                &p, &err));
    CHECK(p.sections.size() == 1 && p.sections[0].keys.size() == 1 && p.sections[0].keys[0].text == "A\"B:C,D");
    PresetLoadResult r = applyPC1600Preset(m, p);
    CHECK(r.ok);

    // Scan FBB0H-FBFFH for the typed run. The ROM stores the edit line
    // here as plain ASCII; ENTER may have cleared or advanced it, so accept
    // the sequence appearing anywhere in the window.
    std::string buf;
    for (uint16_t a = 0xFBB0; a <= 0xFBFF; ++a) buf.push_back(static_cast<char>(m.memory().read(a)));
    bool found = buf.find("A\"B:C,D") != std::string::npos;
    if (!found)
        std::fprintf(stderr, "  input buffer did not contain the shifted run (got: \"%s\")\n", buf.c_str());
    CHECK(found);
}

// Functional: a typed `program:` (`text: |`) block loads after a `key: mode`
// step has put the machine in PRO mode. We can't assert the program is
// stored (locating the PC-1600 native-BASIC program store needs ROM
// disassembly), but the load must complete cleanly with nothing rejected.
void test_loader_applies_basic_text_program() {
    PC1600Machine m;
    if (!loadPC1600Roms(m)) {
        std::fprintf(stderr, "SKIP test_loader_applies_basic_text_program: PC-1600 ROM images not found\n");
        return;
    }
    PresetFile p;
    std::string err;
    CHECK(parse(
        "model: PC-1600\n"
        "keys:\n"
        "  - key: mode\n"
        "program:\n"
        "  text: |\n"
        "    10 PRINT 1\n"
        "    20 GOTO 10\n",
        &p, &err));
    PresetLoadResult r = applyPC1600Preset(m, p);
    CHECK(r.ok);
    CHECK(r.rejectedBasicLines.empty());
    CHECK(r.error.empty());
}

void test_loader_rejects_unknown_key_defensively() {
    // The parser normally catches a bad key, but the loader's own
    // validation must hold too (hand-build a step it never saw).
    PresetFile p;
    p.model = "PC-1600";
    PresetSection s;
    s.kind = PresetSection::Kind::Keys;
    PresetStep step;
    step.kind = PresetStep::Kind::Key;
    step.text = "#nope";
    s.keys.push_back(step);
    p.sections.push_back(s);

    PC1600Machine m;
    loadPC1600Roms(m);  // optional: an empty bus boots too, only slower
    PresetLoadResult r = applyPC1600Preset(m, p);
    CHECK(!r.ok);
    CHECK(!r.error.empty());
}

void test_loader_ce150_plotter_needs_rom_path() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nplotter: CE-150\n", &p, &err));

    PC1600Machine m;
    // No romDirs passed -> a clear error, not a crash / silent skip.
    PresetLoadResult r = applyPC1600Preset(m, p);
    CHECK(!r.ok);
    CHECK(r.error.find("CE-150.ROM") != std::string::npos);
    CHECK(!m.ce150Attached());
}

void test_loader_ce150_plotter_attaches_with_rom_path() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nplotter: CE-150\n", &p, &err));

    PC1600Machine m;
    loadPC1600Roms(m);  // optional: an empty bus boots too, only slower
    PresetLoadResult r = applyPC1600Preset(m, p, {}, ".", ".", {}, {"roms"});
    if (!r.ok) {
        // Skip gracefully if the ROM asset isn't reachable from the cwd.
        std::fprintf(stderr, "SKIP test_loader_ce150_plotter_attaches_with_rom_path: %s\n",
                     r.error.c_str());
        return;
    }
    CHECK(m.ce150Attached());
}

void test_loader_ce1600p_plotter_needs_rom_path() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nplotter: CE-1600P\n", &p, &err));

    PC1600Machine m;
    // No romDirs passed -> a clear error, not a crash / silent skip.
    PresetLoadResult r = applyPC1600Preset(m, p);
    CHECK(!r.ok);
    CHECK(r.error.find("CE1600P") != std::string::npos);
    CHECK(!m.ce1600pAttached());
}

// `floppy:` without `plotter: CE-1600P` is a parse error -- the CE-1600F
// attaches only as a union with the CE-1600P.
void test_parser_rejects_floppy_without_ce1600p() {
    PresetFile p;
    std::string err;
    CHECK(!parse("model: PC-1600\nfloppy: mydisk\n", &p, &err));
    CHECK(err.find("floppy") != std::string::npos);

    CHECK(!parse("model: PC-1600\nplotter: CE-150\nfloppy: mydisk\n", &p, &err));
    CHECK(err.find("floppy") != std::string::npos);
}

// `floppy:` is a PC-1600-only field.
void test_parser_rejects_floppy_on_pc1500() {
    PresetFile p;
    std::string err;
    CHECK(!parse("model: PC-1500\nfloppy: mydisk\n", &p, &err));
    CHECK(err.find("floppy") != std::string::npos);
}

// `floppy: <name>` alongside `plotter: CE-1600P` parses and round-trips
// verbatim into PresetFile::floppy.
void test_parser_accepts_floppy_with_ce1600p() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nplotter: CE-1600P\nfloppy: mydisk\n", &p, &err));
    CHECK(p.floppy == "mydisk");
    CHECK(p.floppySide == 0);
    CHECK(p.plotter == "ce1600p");
}

// `floppy: <name>,A`/`,B` (case-insensitive) strips the side suffix into
// floppySide and leaves floppy as the bare disk name.
void test_parser_accepts_floppy_side_suffix() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nplotter: CE-1600P\nfloppy: mydisk,B\n", &p, &err));
    CHECK(p.floppy == "mydisk");
    CHECK(p.floppySide == 1);

    CHECK(parse("model: PC-1600\nplotter: CE-1600P\nfloppy: mydisk,a\n", &p, &err));
    CHECK(p.floppy == "mydisk");
    CHECK(p.floppySide == 0);

    CHECK(!parse("model: PC-1600\nplotter: CE-1600P\nfloppy: mydisk,Q\n", &p, &err));
    CHECK(err.find("side") != std::string::npos);
}

// Functional: attaching CE-1600P with no `floppy:` key leaves the drive
// empty -- the same "–empty–" default as the GUI.
void test_loader_ce1600p_with_no_floppy_key_leaves_drive_empty() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nplotter: CE-1600P\n", &p, &err));

    PC1600Machine m;
    loadPC1600Roms(m);  // optional: an empty bus boots too, only slower
    PresetLoadResult r = applyPC1600Preset(m, p, {}, ".", ".", {}, {"roms"});
    if (!r.ok) {
        std::fprintf(stderr, "SKIP test_loader_ce1600p_with_no_floppy_key_leaves_drive_empty: %s\n", r.error.c_str());
        return;
    }
    CHECK(m.ce1600fAttached());
    CHECK(r.floppyImageLabel.empty());
    CHECK(!m.ce1600fHasDisk());
}

// Functional: `floppy: <name>` resolves the `*.floppy.yaml` declaring that
// disk-name in `moduleDir` and loads it into the union-attached CE1600FCard.
void test_loader_floppy_key_loads_named_disk_image() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nplotter: CE-1600P\nfloppy: mydisk\n", &p, &err));

    std::vector<uint8_t> diskImage(CE1600FCard::kImageSize, 0x5A);
    CHECK(writeTextFile("/tmp/mydisk-file.floppy.yaml", formatFloppyFile("mydisk", diskImage)));

    PC1600Machine m;
    loadPC1600Roms(m);  // optional: an empty bus boots too, only slower
    PresetLoadResult r = applyPC1600Preset(m, p, {}, ".", "/tmp", {}, {"roms"});
    if (!r.ok) {
        std::fprintf(stderr, "SKIP test_loader_floppy_key_loads_named_disk_image: %s\n", r.error.c_str());
        return;
    }
    CHECK(m.ce1600fAttached());
    CHECK(r.floppyImageLabel == "mydisk");
    CHECK(r.floppyResolvedPath == "/tmp/mydisk-file.floppy.yaml");
    const auto image = m.ce1600fDiskImage();
    CHECK(image.size() == CE1600FCard::kImageSize);
    CHECK(image[0] == 0x5A);
    CHECK(image[CE1600FCard::kImageSize - 1] == 0x5A);
}

// `floppy: <name>,B` selects side B after loading -- loadImage() itself
// always resets to side A, so the preset loader must apply the suffix
// after attach.
void test_loader_floppy_key_side_suffix_selects_side_b() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nplotter: CE-1600P\nfloppy: mydiskb,B\n", &p, &err));

    std::vector<uint8_t> diskImage(CE1600FCard::kImageSize, 0x33);
    CHECK(writeTextFile("/tmp/mydiskb.floppy.yaml", formatFloppyFile("mydiskb", diskImage)));

    PC1600Machine m;
    loadPC1600Roms(m);  // optional: an empty bus boots too, only slower
    PresetLoadResult r = applyPC1600Preset(m, p, {}, ".", "/tmp", {}, {"roms"});
    if (!r.ok) {
        std::fprintf(stderr, "SKIP test_loader_floppy_key_side_suffix_selects_side_b: %s\n", r.error.c_str());
        return;
    }
    CHECK(m.ce1600fAttached());
    CHECK(m.ce1600fSide() == 1);
}

// A `floppy:` name that no `*.floppy.yaml` declares is a
// clear error, not a crash or a silent blank disk.
void test_loader_floppy_key_missing_file_is_an_error() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nplotter: CE-1600P\nfloppy: nosuchdisk\n", &p, &err));

    PC1600Machine m;
    PresetLoadResult r = applyPC1600Preset(m, p, {}, ".", "/tmp", {}, {"roms"});
    CHECK(!r.ok);
    CHECK(r.error.find("nosuchdisk") != std::string::npos);
    CHECK(!m.ce1600fAttached());
}

// A `saveas:` step invokes onSaveAs with the right target/name, at the
// right point in step order (after the preceding steps have already run).
void test_loader_saveas_step_invokes_callback() {
    PresetFile p;
    std::string err;
    CHECK(parse(
        "model: PC-1600\n"
        "keys:\n"
        "  - type: 1\n"
        "  - saveas: live slot-2:My Card\n"
        "  - saveas: template floppy:file:My Disk.floppy.yaml\n",
        &p, &err));

    PC1600Machine m;
    loadPC1600Roms(m);  // optional: an empty bus boots too, only slower
    std::vector<PresetSaveAsRequest> calls;
    PresetSaveAsFn onSaveAs = [&](const PresetSaveAsRequest& request, std::string*) {
        calls.push_back(request);
        return true;
    };
    PresetLoadResult r =
        applyPC1600Preset(m, p, {}, ".", ".", {}, {}, {}, {}, onSaveAs);
    CHECK(r.ok);
    CHECK(calls.size() == 2);
    if (calls.size() == 2) {
        CHECK(calls[0].target == PresetStep::SaveAsTarget::Slot2);
        CHECK(calls[0].name == "My Card");
        CHECK(calls[0].path.empty());
        CHECK(!calls[0].isTemplate);
        CHECK(calls[1].target == PresetStep::SaveAsTarget::Floppy);
        CHECK(calls[1].name == "My Disk");
        CHECK(calls[1].path == "/tmp/My Disk.floppy.yaml");
        CHECK(calls[1].isTemplate);
    }
}

// An onSaveAs failure surfaces its error and stops the preset, exactly
// like any other step failure.
void test_loader_saveas_step_failure_stops_preset() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nkeys:\n  - saveas: live slot-1:Bad\n  - type: 1\n", &p, &err));

    PC1600Machine m;
    loadPC1600Roms(m);  // optional: an empty bus boots too, only slower
    PresetSaveAsFn onSaveAs = [](const PresetSaveAsRequest&, std::string* error) {
        *error = "disk full";
        return false;
    };
    PresetLoadResult r =
        applyPC1600Preset(m, p, {}, ".", ".", {}, {}, {}, {}, onSaveAs);
    CHECK(!r.ok);
    CHECK(r.error.find("disk full") != std::string::npos);
}

// With no onSaveAs callback given, a `saveas:` step is a logged no-op --
// the preset still succeeds.
void test_loader_saveas_step_without_callback_is_noop() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nkeys:\n  - saveas: live slot-1:Whatever\n", &p, &err));

    PC1600Machine m;
    loadPC1600Roms(m);  // optional: an empty bus boots too, only slower
    PresetLoadResult r = applyPC1600Preset(m, p);
    CHECK(r.ok);
}

} // namespace

// `floppy-file:` and `host-drive:` resolve against the preset's directory;
// `~/` is the home directory. One disk key at most; the usual scope rules.
void test_parser_floppy_file_and_host_drive() {
    std::filesystem::create_directories("/tmp/calcu_preset_dir");
    PresetFile p;
    std::string err;
    CHECK(parsePresetString("model: PC-1600\nplotter: CE-1600P\nfloppy-file: disks/P.floppy.yaml,B\nhost-drive: S3\n",
                            "/tmp/calcu_preset_dir/x.pc1600", &p, &err));
    CHECK(p.floppyFile == "/tmp/calcu_preset_dir/disks/P.floppy.yaml");
    CHECK(p.floppy.empty());
    CHECK(p.floppySide == 1);
    CHECK(p.hostDrive == "/tmp/calcu_preset_dir/S3");
    const char* home = std::getenv("HOME");
    CHECK(parsePresetString("model: PC-1600\nhost-drive: ~/share\n", "/tmp/calcu_preset_dir/x.pc1600", &p, &err));
    CHECK(home && p.hostDrive == std::string(home) + "/share");

    CHECK(!parse("model: PC-1600\nplotter: CE-1600P\nfloppy: a\nfloppy-file: b.floppy.yaml\n", &p, &err));
    CHECK(err.find("only one") != std::string::npos);
    CHECK(!parse("model: PC-1600\nfloppy-file: b.floppy.yaml\n", &p, &err));
    CHECK(err.find("floppy-file") != std::string::npos);
    CHECK(!parse("model: PC-1500\nhost-drive: S3\n", &p, &err));
    CHECK(err.find("host-drive") != std::string::npos);
}

// End to end with the ROMs: `floppy-file:` loads the disk, `host-drive:`
// mounts S3: before the boot, and `saveas: template ... file:` writes
// template files through PC1600PresetMedia.hpp.
void test_loader_host_drive_floppy_file_and_template_saves() {
    char tmpl[] = "/tmp/pc1600_preset_media_XXXXXX";
    const char* dir = mkdtemp(tmpl);
    CHECK(dir != nullptr);
    if (!dir) return;
    const std::string d(dir);
    std::filesystem::create_directories(d + "/S3");
    const std::string presetPath = d + "/make.pc1600";
    const std::string floppyPath = std::filesystem::absolute("Qt6/resources/cards/formatted.floppy.yaml").string();
    PresetFile p;
    std::string err;
    CHECK(parsePresetString("model: PC-1600\n"
                            "plotter: CE-1600P\n"
                            "floppy-file: " + floppyPath + "\n"
                            "host-drive: S3\n"
                            "slot-2: CE-1601M\n"
                            "keys:\n"
                            "  - key: mode\n"
                            "  - type: 10 END\n"
                            "  - key: mode\n"
                            "  - type: SAVE\"S3:T.BAS\"\n"
                            "  - saveas: template slot-2:file:Card.card.yaml\n"
                            "  - saveas: template floppy:file:Disk.floppy.yaml\n"
                            "  - saveas: live floppy:file:Live.floppy.yaml\n",
                            presetPath, &p, &err));
    CHECK(err.empty());

    PC1600Machine m;
    if (!loadPC1600Roms(m)) {
        std::fprintf(stderr, "SKIP test_loader_host_drive_floppy_file_and_template_saves: ROM images not found\n");
        return;
    }
    PresetLoadResult armedResult;
    const PresetArmedFn onArmed = [&](const PresetLoadResult& r) { armedResult = r; };
    const PresetSaveAsFn onSaveAs = [&](const PresetSaveAsRequest& request, std::string* e) {
        return savePC1600PresetMedia(m, request, "", armedResult.slot1ResolvedPath, armedResult.slot2ResolvedPath, e);
    };
    PresetLoadResult r = applyPC1600Preset(m, p, {}, ".", "Qt6/resources/cards", {},
                                           {"roms", "firmware/pc1600-hostdrive"}, {}, onArmed, onSaveAs);
    CHECK(r.ok);
    if (!r.ok) std::fprintf(stderr, "  error: %s\n", r.error.c_str());
    CHECK(m.hostDriveAttached());
    CHECK(m.hostDriveDirectory() == std::filesystem::path(d + "/S3"));
    CHECK(r.floppyResolvedPath == floppyPath);
    CHECK(m.ce1600fHasDisk());
    CHECK(std::filesystem::exists(d + "/S3/T.BAS"));

    MemoryCardCatalogEntry card;
    CHECK(readMemoryCardCatalogEntry(d + "/Card.card.yaml", &card, nullptr));
    CHECK(card.moduleName == "Card");
    CHECK(card.isTemplate);
    FloppyFile disk;
    CHECK(readFloppyFile(d + "/Disk.floppy.yaml", &disk, &err));
    CHECK(disk.diskName == "Disk");
    CHECK(disk.isTemplate);
    FloppyFile live;
    CHECK(readFloppyFile(d + "/Live.floppy.yaml", &live, &err));
    CHECK(!live.isTemplate);
    std::filesystem::remove_all(d);
}

// `bus-rom:` end to end: a ROM in bank 7 shadows the host drive's bundled
// ROM. The bundled ROM's own bytes as a bus ROM keep S3: working; a blank
// bus ROM hides the drive from the module scan, so the SAVE lands nowhere.
void test_loader_bus_rom_shadows_the_host_drive_rom() {
    for (const bool blank : {false, true}) {
        char tmpl[] = "/tmp/pc1600_bus_rom_XXXXXX";
        const char* dir = mkdtemp(tmpl);
        CHECK(dir != nullptr);
        if (!dir) return;
        const std::string d(dir);
        std::filesystem::create_directories(d + "/S3");
        std::vector<char> rom(0x4000, char(0xFF));
        if (!blank) {
            std::ifstream in("firmware/pc1600-hostdrive/PC1600-P1-B7-HOSTDRIVE.bin", std::ios::binary);
            in.read(rom.data(), std::streamsize(rom.size()));
            CHECK(in.gcount() == 0x4000);
        }
        std::ofstream(d + "/ext.bin", std::ios::binary).write(rom.data(), std::streamsize(rom.size()));
        PresetFile p;
        std::string err;
        CHECK(parsePresetString("model: PC-1600\n"
                                "host-drive: S3\n"
                                "bus-rom:\n"
                                "  - file: ext.bin\n"
                                "    bank: 7\n"
                                "keys:\n"
                                "  - key: mode\n"
                                "  - type: 10 END\n"
                                "  - key: mode\n"
                                "  - type: SAVE\"S3:T.BAS\"\n",
                                d + "/p.pc1600", &p, &err));
        CHECK(p.busRoms.size() == 1 && p.busRoms[0].bank == 7 && p.busRoms[0].path == d + "/ext.bin");
        PC1600Machine m;
        if (!loadPC1600Roms(m)) {
            std::fprintf(stderr, "SKIP test_loader_bus_rom_shadows_the_host_drive_rom: ROM images not found\n");
            return;
        }
        const PresetLoadResult r = applyPC1600Preset(m, p, {}, ".", "Qt6/resources/cards", {},
                                                     {"roms", "firmware/pc1600-hostdrive"});
        CHECK(r.ok);
        if (!r.ok) std::fprintf(stderr, "  error: %s\n", r.error.c_str());
        CHECK(std::filesystem::exists(d + "/S3/T.BAS") == !blank);
        std::filesystem::remove_all(d);
    }
}

void test_parser_bus_rom_forms() {
    PresetFile p;
    std::string err;
    CHECK(parsePresetString("model: PC-1600\n"
                            "bus-rom:\n"
                            "  - file: a.bin\n"
                            "    bank: 6\n"
                            "  - file: /abs/b.bin\n"
                            "    address: 0x8000\n"
                            "    pv: 1\n"
                            "    pu: 0\n"
                            "    me: 0\n",
                            "/tmp/pc1600_bus_rom_forms.pc1600", &p, &err));
    CHECK(err.empty());
    CHECK(p.busRoms.size() == 2);
    if (p.busRoms.size() == 2) {
        CHECK(p.busRoms[0].path == "/tmp/a.bin" && p.busRoms[0].bank == 6);
        CHECK(p.busRoms[1].path == "/abs/b.bin" && p.busRoms[1].bank < 0 && p.busRoms[1].address == 0x8000);
        CHECK(p.busRoms[1].pv == 1 && p.busRoms[1].pu == 0 && !p.busRoms[1].me1 && p.busRoms[1].bank == -1);
    }
    const auto rejects = [&](const std::string& body, const char* what) {
        PresetFile q;
        std::string e;
        const bool ok = parsePresetString(body, "/tmp/pc1600_bus_rom_forms.pc1600", &q, &e);
        CHECK(!ok);
        if (!ok && e.find(what) == std::string::npos) {
            g_fail++;
            std::fprintf(stderr, "FAIL bus-rom error '%s' lacks '%s'\n", e.c_str(), what);
        }
    };
    rejects("model: PC-1600\nbus-rom:\n  - file: a.bin\n    bank: 3\n", "'bank' must be 4-7");
    rejects("model: PC-1600\nbus-rom:\n  - file: a.bin\n", "either 'bank'");
    rejects("model: PC-1600\nbus-rom:\n  - file: a.bin\n    bank: 7\n    address: 0x4000\n", "either 'bank'");
    rejects("model: PC-1600\nbus-rom:\n  - file: a.bin\n    bank: 7\n    pv: 1\n", "go with 'address'");
    rejects("model: PC-1600\nbus-rom:\n  - bank: 7\n", "needs 'file'");
    rejects("model: PC-1600\nbus-rom:\n  - file: a.bin\n    address: 0x8000\n    pv: 2\n", "0 or 1");
    rejects("model: PC-1500\nbus-rom:\n  - file: a.bin\n    bank: 7\n", "a PC-1500 ROM takes 'address'");
}

// The cards on their buses: a bus ROM attached first answers before a card
// at the same place, and only inside its window and gates.
void test_bus_rom_cards_shadow_and_gate() {
    PC1600SystemBus bus;
    PC1600HostDriveCard drive;
    std::vector<uint8_t> hostRom(0x4000, 0x11);
    CHECK(drive.loadRom(hostRom.data(), hostRom.size()));
    bus.attach(&drive);
    PC1600BusRomCard ext(std::vector<uint8_t>(0x100, 0x22), 7);
    bus.attachFirst(&ext);
    uint8_t v = 0;
    CHECK(bus.readRom(0x0000, 7, v) && v == 0x22);   // shadowed
    CHECK(bus.readRom(0x0100, 7, v) && v == 0x11);   // past the bus ROM: the drive's
    CHECK(!bus.readRom(0x0000, 6, v));               // other bank: nobody
    CHECK(bus.readIO(0x91, v) && v == 0x00);         // the drive's I/O is untouched

    std::vector<uint8_t> bytes(0x2000);
    for (size_t i = 0; i < bytes.size(); i++) bytes[i] = uint8_t(i);
    BusRomCard rom(bytes, 0x8000, /*me1=*/false, /*pv=*/1, /*pu=*/0);
    PinState pins;
    pins.address = 0x8010;
    pins.pin[2] = true;  // PV
    pins.pin[3] = false; // PU
    CHECK(rom.respondsToRead(pins, v) && v == 0x10);
    pins.pin[3] = true;
    CHECK(!rom.respondsToRead(pins, v));             // PU gate
    pins.pin[3] = false;
    pins.pin[2] = false;
    CHECK(!rom.respondsToRead(pins, v));             // PV gate
    pins.pin[2] = true;
    pins.me1 = true;
    CHECK(!rom.respondsToRead(pins, v));             // ME1
    pins.me1 = false;
    pins.address = 0xA000;
    CHECK(!rom.respondsToRead(pins, v));             // past the end
    pins.address = 0x7FFF;
    CHECK(!rom.respondsToRead(pins, v));             // below the base
    pins.address = 0x8000;
    pins.forWrite = true;
    CHECK(!rom.respondsToRead(pins, v) && !rom.respondsToWrite(pins, 0).claimed);
}

int run_pc1600_preset_tests() {
    test_parser_bus_rom_forms();
    test_bus_rom_cards_shadow_and_gate();
    test_loader_bus_rom_shadows_the_host_drive_rom();
    test_parser_floppy_file_and_host_drive();
    test_loader_host_drive_floppy_file_and_template_saves();
    test_parser_accepts_pc1600_with_slot_and_keys();
    test_parser_rejects_multichar_key();
    test_parser_both_slots();
    test_parser_pc1600_machine_binary();
    test_parser_rejects_cross_model_fields();
    test_parser_accepts_basic_text_program();
    test_loader_reports_overlong_basic_line();
    test_loader_applies_ce155_and_type_step();
    test_type_step_rejects_untypeable_char();
    test_type_step_shifted_punctuation_reaches_input_buffer();
    test_loader_applies_basic_text_program();
    test_loader_machine_binary_header();
    test_loader_machine_binary_length_mismatch();
    test_loader_machine_binary_headerless();
    test_loader_machine_binary_headerless_address_only_and_ce158();
    test_loader_machine_binary_autorun();
    test_loader_machine_binary_autorun_slot2();
    test_loader_machine_binary_mode1();
    test_loader_machine_binary_title_s1();
    test_loader_rejects_unknown_key_defensively();
    test_loader_ce150_plotter_needs_rom_path();
    test_loader_ce150_plotter_attaches_with_rom_path();
    test_loader_ce1600p_plotter_needs_rom_path();
    test_parser_rejects_floppy_without_ce1600p();
    test_parser_rejects_floppy_on_pc1500();
    test_parser_accepts_floppy_with_ce1600p();
    test_parser_accepts_floppy_side_suffix();
    test_loader_ce1600p_with_no_floppy_key_leaves_drive_empty();
    test_loader_floppy_key_loads_named_disk_image();
    test_loader_floppy_key_side_suffix_selects_side_b();
    test_loader_floppy_key_missing_file_is_an_error();
    test_loader_saveas_step_invokes_callback();
    test_loader_saveas_step_failure_stops_preset();
    test_loader_saveas_step_without_callback_is_noop();

    std::printf("pc1600_preset_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
