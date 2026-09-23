// Headless C++ tests for the `model: PC-1600` preset path: the parser
// extension in PresetFile.cpp (memory-expansion-1:/-2:, PC-1600 model)
// and Core/PC1600/PC1600PresetLoader.cpp. Same no-framework, assert-and-
// tally style as lh5801_tests.cpp.
//
// Build & run: see tools/run_tests.sh


#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <unistd.h> // mkdtemp

#include "../Connector/CE1600FCard.hpp"
#include "../Connector/FloppyImageFile.hpp"
#include "../PC1600/PC1600Machine.hpp"
#include "../PC1600/PC1600PresetLoader.hpp"
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
        "memory-expansion-1:\n"
        "  - modulespec: CE-155\n"
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
        "memory-expansion-1:\n"
        "  - modulespec: CE-1600M\n"
        "memory-expansion-2:\n"
        "  - modulespec: CE-1601M\n",
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
    // PC-1600 preset with the unsuffixed memory-expansion: block.
    CHECK(!parse("model: PC-1600\nmemory-expansion:\n  - modulespec: CE-155\n", &p, &err));
    // PC-1600 `format: binary` (machine-language) without a target slot --
    // rejected; a slot is mandatory (see test_parser_pc1600_machine_binary).
    // Fresh PresetFile: parsePresetFile() merges into *out rather than
    // resetting it, so a stale memory-expansion from an earlier case would
    // otherwise mask the slot error.
    p = PresetFile{};
    CHECK(!parse("model: PC-1600\nprogram:\n  format: binary\n  path: x.bin\n  address: 0x8000\n", &p, &err));
    CHECK(err.find("slot") != std::string::npos);
    // PC-1500 preset with a per-slot block.
    p = PresetFile{};
    CHECK(!parse("model: PC-1500A\nmemory-expansion-1:\n  - modulespec: CE-155\n", &p, &err));
    // The built-in `- module: <name>` form is gone.
    p = PresetFile{};
    CHECK(!parse("model: PC-1600\nmemory-expansion-1:\n  - module: ce155\n", &p, &err));
    CHECK(err.find("modulespec") != std::string::npos);
}

void test_parser_pc1600_machine_binary() {
    // parsePresetFile() merges into *out instead of resetting it, so each
    // case that inspects the parse result gets its own PresetFile.
    std::string err;

    // `slot: S0` accepted; `address` / `length` optional (a header may
    // supply them). `slot` string is case-insensitive.
    {
        PresetFile p;
        CHECK(parse("model: PC-1600\nprogram:\n  format: binary\n  slot: s0\n  path: x.bin\n", &p, &err));
        CHECK(p.sections.size() == 1);
        CHECK(p.sections[0].program.format == PresetProgram::Format::Binary);
        CHECK(p.sections[0].program.slot == PresetProgram::Slot::S0);
        CHECK(!p.sections[0].program.hasAddress);
        CHECK(!p.sections[0].program.hasLength);
    }

    // `address` / `length` overrides, both bases.
    {
        PresetFile p;
        CHECK(parse("model: PC-1600\nprogram:\n  format: binary\n  slot: S2\n  path: x.bin\n"
                    "  address: 0x8100\n  length: 0x40\n",
                    &p, &err));
        CHECK(p.sections[0].program.slot == PresetProgram::Slot::S2);
        CHECK(p.sections[0].program.hasAddress && p.sections[0].program.address == 0x8100);
        CHECK(p.sections[0].program.hasLength && p.sections[0].program.length == 0x40);
    }
    {
        PresetFile p;
        CHECK(parse("model: PC-1600\nprogram:\n  format: binary\n  slot: S1\n  path: x.bin\n"
                    "  length: 256\n",
                    &p, &err));
        CHECK(p.sections[0].program.length == 256);
    }

    // Bad slot / bad length / slot on the wrong format / slot on PC-1500.
    {
        PresetFile p;
        CHECK(!parse("model: PC-1600\nprogram:\n  format: binary\n  slot: S3\n  path: x.bin\n", &p, &err));
    }
    {
        PresetFile p;
        CHECK(!parse("model: PC-1600\nprogram:\n  format: binary\n  slot: S0\n  path: x.bin\n"
                     "  length: 12x\n",
                     &p, &err));
    }
    {
        PresetFile p;
        CHECK(!parse("model: PC-1600\nprogram:\n  format: basic-binary\n  slot: S0\n  path: x.bas\n",
                     &p, &err));
        CHECK(err.find("slot") != std::string::npos);
    }
    {
        PresetFile p;
        CHECK(!parse("model: PC-1500A\nprogram:\n  format: binary\n  slot: S0\n  path: x.bin\n"
                     "  address: 0x40C5\n",
                     &p, &err));
        CHECK(err.find("PC-1600") != std::string::npos);
    }

    // PC-1500 `format: binary` still needs an address and rejects `length`.
    {
        PresetFile p;
        CHECK(!parse("model: PC-1500A\nprogram:\n  format: binary\n  path: x.bin\n", &p, &err));
    }
    {
        PresetFile p;
        CHECK(!parse("model: PC-1500A\nprogram:\n  format: binary\n  path: x.bin\n  address: 0x40C5\n"
                     "  length: 8\n",
                     &p, &err));
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

// Functional: a `program: format: binary` block with a PC-1600 ML header
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
    CHECK(parse("model: PC-1600\nprogram:\n  format: binary\n  slot: S0\n  path: " + bin + "\n",
                &p, &err));
    PC1600PresetLoadResult r = applyPC1600Preset(m, p);
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
    CHECK(parse("model: PC-1600\nprogram:\n  format: binary\n  slot: S0\n  path: " + bin + "\n",
                &p, &err));
    PC1600PresetLoadResult r = applyPC1600Preset(m, p);
    CHECK(!r.ok);
    CHECK(r.error.find("length") != std::string::npos);

    // Same file, explicit length: 4 -> loads all four bytes.
    PC1600Machine m2;
    loadPC1600Roms(m2);
    PresetFile p2;
    CHECK(parse("model: PC-1600\nprogram:\n  format: binary\n  slot: S0\n  path: " + bin +
                    "\n  length: 4\n",
                &p2, &err));
    PC1600PresetLoadResult r2 = applyPC1600Preset(m2, p2);
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
    CHECK(parse("model: PC-1600\nprogram:\n  format: binary\n  slot: S0\n  path: " + bin + "\n",
                &p, &err));
    PC1600PresetLoadResult r = applyPC1600Preset(m, p);
    CHECK(!r.ok);
    CHECK(r.error.find("required") != std::string::npos);

    PC1600Machine m2;
    loadPC1600Roms(m2);
    PresetFile p2;
    CHECK(parse("model: PC-1600\nprogram:\n  format: binary\n  slot: S0\n  path: " + bin +
                    "\n  address: 0xD200\n  length: 4\n",
                &p2, &err));
    PC1600PresetLoadResult r2 = applyPC1600Preset(m2, p2);
    CHECK(r2.ok);
    CHECK(m2.debugPeek(0xD200) == 0xAA);
    CHECK(m2.debugPeek(0xD203) == 0xDD);
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
    CHECK(parse("model: PC-1600\nprogram:\n  format: binary\n  slot: S0\n  path: " + bin + "\n",
                &p, &err));
    PC1600PresetLoadResult r = applyPC1600Preset(m, p);
    CHECK(r.ok);
    CHECK(r.error.empty());
    CHECK(m.debugPeek(0xD300) == 0xC9);
}

void test_parser_accepts_basic_text_program() {
    PresetFile p;
    std::string err;
    CHECK(parse(
        "model: PC-1600\n"
        "keys:\n"
        "  - key: mode\n"
        "program:\n"
        "  format: basic-text\n"
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
        "  format: basic-text\n"
        "  text: |\n"
        "    10 PRINT 1\n"
        "    " + longLine + "\n"
        "    30 END\n",
        &p, &err));

    PC1600PresetLoadResult r = applyPC1600Preset(m, p);
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
        "memory-expansion-1:\n"
        "  - modulespec: CE-155\n"
        "keys:\n"
        "  - type: MEM\n",
        &p, &err));

    PC1600Machine m;
    // (No ROM set loaded -- the loader doesn't require it; boot-settle just
    // spins the CPU. Slot wiring + step replay is what we're checking.)
    PC1600PresetLoadResult r = applyPC1600Preset(m, p, {}, ".", "Qt6/resources/cards");
    CHECK(r.ok);
    CHECK(m.slot1Attached());
    CHECK(!m.slot2Attached());

    // The card is live: with page-C bank 0 selected it answers across
    // &A000-&BFFF; &A800 is its pin-17 (S2) block.
    m.memory().writeIO(0x31, 0x00);
    m.memory().write(0xA800, 0x5A);
    CHECK(m.memory().read(0xA800) == 0x5A);
}

void test_loader_accepts_trace_step() {
    // `- trace:` now works for a PC-1600 preset (see
    // presetloader_trace_tests.cpp for the full file-format check). Here:
    // it loads, and the file is created with the TRACE.bin magic. No ROM
    // needed -- boot-settle just spins both CPUs.
    char tmpl[] = "/tmp/calcu1600_pc1600_trace_XXXXXX";
    const char* dir = mkdtemp(tmpl);
    CHECK(dir != nullptr);
    if (!dir) return;

    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\n"
                "keys:\n"
                "  - trace: t.bin\n"
                "  - wait: 0.1\n"
                "  - trace: off\n",
                &p, &err));
    PC1600Machine m;
    PC1600PresetLoadResult r = applyPC1600Preset(m, p, {}, dir);
    CHECK(r.ok);

    std::string path = std::string(dir) + "/t.bin";
    std::ifstream in(path, std::ios::binary);
    CHECK(static_cast<bool>(in));
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(buf.size() >= 16);
    // 'PC15' little-endian magic, format version 2.
    CHECK(buf.size() >= 6 && buf[0] == 0x35 && buf[1] == 0x31 && buf[2] == 0x43 && buf[3] == 0x50);

    std::remove(path.c_str());
    std::remove(dir);
}

void test_type_step_rejects_untypeable_char() {
    PresetFile p;
    std::string err;
    // A control byte (0x01) has no PC-1600 key.
    CHECK(parse(std::string("model: PC-1600\nkeys:\n  - type: a\x01""b\n"), &p, &err));
    PC1600Machine m;
    PC1600PresetLoadResult r = applyPC1600Preset(m, p);
    CHECK(!r.ok);
    CHECK(!r.error.empty());
}

void test_type_step_accepts_shifted_punctuation() {
    // `INIT"S2:","M"` -- the '"' ':' ',' are SHIFT + a base key on the
    // PC-1600 (the ROM's key-code table does the translation). The loader
    // must resolve every such character and apply the step.
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\n"
                "keys:\n"
                "  - type: INIT\"S2:\",\"M\"\n",
                &p, &err));
    CHECK(p.sections.size() == 1 && p.sections[0].keys.size() == 1);
    CHECK(p.sections[0].keys[0].text == "INIT\"S2:\",\"M\"");
    PC1600Machine m;
    PC1600PresetLoadResult r = applyPC1600Preset(m, p);
    CHECK(r.ok);
    CHECK(r.error.empty());
}

// Functional: with the real ROM booting, a `type:` line with shifted
// punctuation lands the right ASCII in the console input buffer
// (FBB0H-FBFFH, PC-1600-Work-Area-Map.md) -- i.e. SHIFT + base key really
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
    PC1600PresetLoadResult r = applyPC1600Preset(m, p);
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

// Functional: `type:` is case-sensitive -- a lowercase run reaches the
// console input buffer as lowercase (SHIFT-tap path), not silently
// uppercased the way the pre-typer raw-keystroke `type:` did.
void test_type_step_is_case_sensitive() {
    PC1600Machine m;
    if (!loadPC1600Roms(m)) {
        std::fprintf(stderr, "SKIP test_type_step_is_case_sensitive: PC-1600 ROM images not found\n");
        return;
    }
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nkeys:\n  - type: abcXYZ\n", &p, &err));
    PC1600PresetLoadResult r = applyPC1600Preset(m, p);
    CHECK(r.ok);

    std::string buf;
    for (uint16_t a = 0xFBB0; a <= 0xFBFF; ++a) buf.push_back(static_cast<char>(m.memory().read(a)));
    CHECK(buf.find("abcXYZ") != std::string::npos);
    CHECK(buf.find("ABCXYZ") == std::string::npos);
}

// Functional: a `program:` (basic-text) block loads after a `key: mode`
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
        "  format: basic-text\n"
        "  text: |\n"
        "    10 PRINT 1\n"
        "    20 GOTO 10\n",
        &p, &err));
    PC1600PresetLoadResult r = applyPC1600Preset(m, p);
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
    PC1600PresetLoadResult r = applyPC1600Preset(m, p);
    CHECK(!r.ok);
    CHECK(!r.error.empty());
}

void test_loader_ce150_plotter_needs_rom_path() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nplotter: ce150\n", &p, &err));

    PC1600Machine m;
    // No romDirs passed -> a clear error, not a crash / silent skip.
    PC1600PresetLoadResult r = applyPC1600Preset(m, p);
    CHECK(!r.ok);
    CHECK(r.error.find("CE-150.ROM") != std::string::npos);
    CHECK(!m.ce150Attached());
}

void test_loader_ce150_plotter_attaches_with_rom_path() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nplotter: ce150\n", &p, &err));

    PC1600Machine m;
    PC1600PresetLoadResult r = applyPC1600Preset(m, p, {}, ".", ".", {}, {"roms"});
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
    CHECK(parse("model: PC-1600\nplotter: ce1600p\n", &p, &err));

    PC1600Machine m;
    // No romDirs passed -> a clear error, not a crash / silent skip.
    PC1600PresetLoadResult r = applyPC1600Preset(m, p);
    CHECK(!r.ok);
    CHECK(r.error.find("CE1600P") != std::string::npos);
    CHECK(!m.ce1600pAttached());
}

void test_loader_no_plotter_key_attaches_nothing() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nkeys:\n  - type: 1\n", &p, &err));

    PC1600Machine m;
    PC1600PresetLoadResult r = applyPC1600Preset(m, p);
    CHECK(r.ok);
    CHECK(!m.ce1600pAttached());
}

// `floppy:` without `plotter: ce1600p` is a parse error -- the CE-1600F
// attaches only as a union with the CE-1600P.
void test_parser_rejects_floppy_without_ce1600p() {
    PresetFile p;
    std::string err;
    CHECK(!parse("model: PC-1600\nfloppy: mydisk\n", &p, &err));
    CHECK(err.find("floppy") != std::string::npos);

    CHECK(!parse("model: PC-1600\nplotter: ce150\nfloppy: mydisk\n", &p, &err));
    CHECK(err.find("floppy") != std::string::npos);
}

// `floppy:` is a PC-1600-only field.
void test_parser_rejects_floppy_on_pc1500() {
    PresetFile p;
    std::string err;
    CHECK(!parse("model: PC-1500\nfloppy: mydisk\n", &p, &err));
    CHECK(err.find("floppy") != std::string::npos);
}

// `floppy: <name>` alongside `plotter: ce1600p` parses and round-trips
// verbatim into PresetFile::floppy.
void test_parser_accepts_floppy_with_ce1600p() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nplotter: ce1600p\nfloppy: mydisk\n", &p, &err));
    CHECK(p.floppy == "mydisk");
    CHECK(p.floppySide == 0);
    CHECK(p.plotter == "ce1600p");
}

// `floppy: <name>,A`/`,B` (case-insensitive) strips the side suffix into
// floppySide and leaves floppy as the bare disk name.
void test_parser_accepts_floppy_side_suffix() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nplotter: ce1600p\nfloppy: mydisk,B\n", &p, &err));
    CHECK(p.floppy == "mydisk");
    CHECK(p.floppySide == 1);

    CHECK(parse("model: PC-1600\nplotter: ce1600p\nfloppy: mydisk,a\n", &p, &err));
    CHECK(p.floppy == "mydisk");
    CHECK(p.floppySide == 0);

    CHECK(!parse("model: PC-1600\nplotter: ce1600p\nfloppy: mydisk,Q\n", &p, &err));
    CHECK(err.find("side") != std::string::npos);
}

// Functional: attaching CE-1600P with no `floppy:` key leaves the drive
// empty -- the same "–empty–" default as the GUI.
void test_loader_ce1600p_with_no_floppy_key_leaves_drive_empty() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nplotter: ce1600p\n", &p, &err));

    PC1600Machine m;
    PC1600PresetLoadResult r = applyPC1600Preset(m, p, {}, ".", ".", {}, {"roms"});
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
    CHECK(parse("model: PC-1600\nplotter: ce1600p\nfloppy: mydisk\n", &p, &err));

    std::vector<uint8_t> diskImage(CE1600FCard::kImageSize, 0x5A);
    CHECK(writeTextFile("/tmp/mydisk-file.floppy.yaml", formatFloppyFile("mydisk", diskImage)));

    PC1600Machine m;
    PC1600PresetLoadResult r = applyPC1600Preset(m, p, {}, ".", "/tmp", {}, {"roms"});
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
    CHECK(parse("model: PC-1600\nplotter: ce1600p\nfloppy: mydiskb,B\n", &p, &err));

    std::vector<uint8_t> diskImage(CE1600FCard::kImageSize, 0x33);
    CHECK(writeTextFile("/tmp/mydiskb.floppy.yaml", formatFloppyFile("mydiskb", diskImage)));

    PC1600Machine m;
    PC1600PresetLoadResult r = applyPC1600Preset(m, p, {}, ".", "/tmp", {}, {"roms"});
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
    CHECK(parse("model: PC-1600\nplotter: ce1600p\nfloppy: nosuchdisk\n", &p, &err));

    PC1600Machine m;
    PC1600PresetLoadResult r = applyPC1600Preset(m, p, {}, ".", "/tmp", {}, {"roms"});
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
        "  - saveas: s2:My Card\n"
        "  - saveas: floppy:My Disk\n",
        &p, &err));

    PC1600Machine m;
    std::vector<std::pair<PresetStep::SaveAsTarget, std::string>> calls;
    PresetSaveAsFn onSaveAs = [&](PresetStep::SaveAsTarget target, const std::string& name,
                                        std::string*) {
        calls.push_back({target, name});
        return true;
    };
    PC1600PresetLoadResult r =
        applyPC1600Preset(m, p, {}, ".", ".", {}, {}, {}, {}, onSaveAs);
    CHECK(r.ok);
    CHECK(calls.size() == 2);
    CHECK(calls[0].first == PresetStep::SaveAsTarget::S2);
    CHECK(calls[0].second == "My Card");
    CHECK(calls[1].first == PresetStep::SaveAsTarget::Floppy);
    CHECK(calls[1].second == "My Disk");
}

// An onSaveAs failure surfaces its error and stops the preset, exactly
// like any other step failure.
void test_loader_saveas_step_failure_stops_preset() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nkeys:\n  - saveas: s1:Bad\n  - type: 1\n", &p, &err));

    PC1600Machine m;
    PresetSaveAsFn onSaveAs = [](PresetStep::SaveAsTarget, const std::string&,
                                       std::string* error) {
        *error = "disk full";
        return false;
    };
    PC1600PresetLoadResult r =
        applyPC1600Preset(m, p, {}, ".", ".", {}, {}, {}, {}, onSaveAs);
    CHECK(!r.ok);
    CHECK(r.error.find("disk full") != std::string::npos);
}

// With no onSaveAs callback given, a `saveas:` step is a logged no-op --
// the preset still succeeds.
void test_loader_saveas_step_without_callback_is_noop() {
    PresetFile p;
    std::string err;
    CHECK(parse("model: PC-1600\nkeys:\n  - saveas: s1:Whatever\n", &p, &err));

    PC1600Machine m;
    PC1600PresetLoadResult r = applyPC1600Preset(m, p);
    CHECK(r.ok);
}

} // namespace

int run_pc1600_preset_tests() {
    test_parser_accepts_pc1600_with_slot_and_keys();
    test_parser_rejects_multichar_key();
    test_parser_both_slots();
    test_parser_pc1600_machine_binary();
    test_parser_rejects_cross_model_fields();
    test_parser_accepts_basic_text_program();
    test_loader_reports_overlong_basic_line();
    test_loader_applies_ce155_and_type_step();
    test_loader_accepts_trace_step();
    test_type_step_rejects_untypeable_char();
    test_type_step_accepts_shifted_punctuation();
    test_type_step_shifted_punctuation_reaches_input_buffer();
    test_type_step_is_case_sensitive();
    test_loader_applies_basic_text_program();
    test_loader_machine_binary_header();
    test_loader_machine_binary_length_mismatch();
    test_loader_machine_binary_headerless();
    test_loader_machine_binary_autorun();
    test_loader_rejects_unknown_key_defensively();
    test_loader_ce150_plotter_needs_rom_path();
    test_loader_ce150_plotter_attaches_with_rom_path();
    test_loader_ce1600p_plotter_needs_rom_path();
    test_loader_no_plotter_key_attaches_nothing();
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
