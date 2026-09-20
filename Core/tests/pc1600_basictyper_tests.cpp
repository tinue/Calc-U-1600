// Headless C++ tests for Core/PC1600/PC1600BasicTyper.cpp -- the PC-1600
// analog of basictyper_tests.cpp.
//
// The PC-1600's native BASIC does not tokenize (a line is stored as plain
// ASCII), so typeBasicProgramText() is a thin loop over typeLine() with an
// up-front line-length guard -- there is no program-end pointer to watch.
// The length-guard logic is covered here without a ROM; the ROM-gated
// tests confirm typed input (case preserved) reaches the ROM's line
// editor. Full "program stored correctly in PRO mode" validation is a
// manual/GUI check -- locating the PC-1600 native-BASIC program store
// needs ROM disassembly and is out of scope here.
//
// ROM-gated tests need the confirmed PC-1600 ROM set at its repo-root path
// (roms/PC1600-*.bin); they SKIP (not fail) when the images are absent.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../PC1600/PC1600BasicTyper.hpp"
#include "../PC1600/PC1600Machine.hpp"
#include "TestRoms.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// Loads the confirmed PC-1600 ROM set; returns false (test skipped) if the
// images aren't at their repo-root path. Mirrors pc1600_preset_tests.cpp.
// No ROM needed: an untypeable character fails the line cleanly with an
// error rather than half-typing it.
void test_typeline_rejects_untypeable_char() {
    PC1600Machine m;
    std::string err;
    CHECK(!typeLine(m, std::string("a\x01""b"), /*pressEnter=*/true, &err));
    CHECK(!err.empty());
}

// No ROM needed: the up-front length guard flags an over-limit line
// without typing it. (Without a ROM the program-end pointer never moves,
// so the fitting lines get flagged too -- that path is covered by the
// ROM-gated tests below; here we only assert the over-limit line is
// caught and never typed.)
void test_typebasicprogram_length_guard() {
    PC1600Machine m; // no ROM: keystrokes just spin the CPU

    std::string tooLong = "10 REM ";
    while (static_cast<int>(tooLong.size()) <= kMaxBasicLineLength) tooLong += 'X';

    PC1600BasicTypeResult r = typeBasicProgramText(m, tooLong + "\n");
    CHECK(!r.ok);
    CHECK(r.rejectedLines.size() == 1);
    CHECK(!r.rejectedLines.empty() && r.rejectedLines[0] == tooLong);
    CHECK(!r.error.empty());
}

// ROM-gated: in PRO mode a fitting program loads (every line advances
// BASPRG_END), and an over-length line is the only one rejected.
void test_typebasicprogram_pro_mode_load() {
    PC1600Machine m;
    if (!bootPC1600(m)) {
        std::fprintf(stderr, "SKIP test_typebasicprogram_pro_mode_load: PC-1600 ROM images not found\n");
        return;
    }
    tapKey(m, "mode"); // RUN -> PRO
    m.runCycles(static_cast<uint64_t>(PC1600Machine::kTStateHz) / 2);

    std::string tooLong = "30 REM ";
    while (static_cast<int>(tooLong.size()) <= kMaxBasicLineLength) tooLong += 'X';

    PC1600BasicTypeResult r = typeBasicProgramText(
        m, "10 PRINT 12345\n20 GOTO 10\n" + tooLong + "\n40 END\n");
    CHECK(!r.ok);                                // the one over-length line
    CHECK(r.rejectedLines.size() == 1);
    CHECK(!r.rejectedLines.empty() && r.rejectedLines[0] == tooLong);
}

// ROM-gated: called in RUN mode (no `key: mode`), the editor stores
// nothing -- every line is reported rejected and the error names PRO mode.
void test_typebasicprogram_detects_run_mode() {
    PC1600Machine m;
    if (!bootPC1600(m)) {
        std::fprintf(stderr, "SKIP test_typebasicprogram_detects_run_mode: PC-1600 ROM images not found\n");
        return;
    }
    PC1600BasicTypeResult r = typeBasicProgramText(m, "10 PRINT 1\n20 PRINT 2\n");
    CHECK(!r.ok);
    CHECK(r.rejectedLines.size() == 2);
    CHECK(r.error.find("PRO mode") != std::string::npos);
}

// typeLine() drives the ROM's line editor: a typed run reaches the console
// input buffer (FBB0H-FBFFH -- the same buffer pc1600_preset_tests.cpp's
// shifted-punctuation test reads), with lowercase preserved via the
// SHIFT-tap path rather than silently uppercased (the pre-typer raw-
// keystroke behaviour). The ROM materialises this buffer on the line
// commit (ENTER), so type WITH enter and read it back after.
void test_typeline_lowercase_reaches_input_buffer() {
    PC1600Machine m;
    if (!bootPC1600(m)) {
        std::fprintf(stderr, "SKIP test_typeline_lowercase_reaches_input_buffer: PC-1600 ROM images not found\n");
        return;
    }
    std::string err;
    CHECK(typeLine(m, "abcXYZ", /*pressEnter=*/true, &err));

    std::string buf;
    for (uint16_t a = 0xFBB0; a <= 0xFBFF; ++a) buf.push_back(static_cast<char>(m.memory().read(a)));
    CHECK(buf.find("abcXYZ") != std::string::npos);
    // ...not the fully-uppercased form.
    CHECK(buf.find("ABCXYZ") == std::string::npos);
}

// No ROM needed: '^' (caret) is typeable -- SHIFT + SPACE on real
// hardware. A `type:` step in a preset can legitimately carry one (e.g. a
// DiskWorks .CFG line written via PRINT#), so the typer must not reject it.
void test_typeline_accepts_caret() {
    PC1600Machine m;
    std::string err;
    CHECK(typeLine(m, "A^B", /*pressEnter=*/true, &err));
    CHECK(err.empty());
}

// ROM-gated: a typed '^' reaches the console input buffer as a real caret
// byte (0x5E), same check shape as the lowercase test above.
void test_typeline_caret_reaches_input_buffer() {
    PC1600Machine m;
    if (!bootPC1600(m)) {
        std::fprintf(stderr, "SKIP test_typeline_caret_reaches_input_buffer: PC-1600 ROM images not found\n");
        return;
    }
    std::string err;
    CHECK(typeLine(m, "X^Y", /*pressEnter=*/true, &err));

    std::string buf;
    for (uint16_t a = 0xFBB0; a <= 0xFBFF; ++a) buf.push_back(static_cast<char>(m.memory().read(a)));
    CHECK(buf.find("X^Y") != std::string::npos);
}

} // namespace

int run_pc1600_basictyper_tests() {
    test_typeline_rejects_untypeable_char();
    test_typeline_accepts_caret();
    test_typeline_caret_reaches_input_buffer();
    test_typebasicprogram_length_guard();
    test_typebasicprogram_pro_mode_load();
    test_typebasicprogram_detects_run_mode();
    test_typeline_lowercase_reaches_input_buffer();

    std::printf("pc1600_basictyper_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
