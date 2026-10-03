// Headless C++ tests for Core/Display/LcdText and the per-model adapters
// (PC1500LcdText.hpp, PC1600LcdText.hpp). The synthetic tests build a
// bitmap from a hand-made font; the ROM-gated ones print through the real
// ROM and read the screen back. ROM-gated tests SKIP (not fail) when
// images are absent.
//
// Build & run: see tools/run_tests.sh

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

#include "../Display/LcdText.hpp"
#include "../PC1500/PC1500BasicTyper.hpp"
#include "../PC1500/PC1500LcdText.hpp"
#include "../PC1500/PC1500PresetLoader.hpp"
#include "../PC1600/PC1600BasicTyper.hpp"
#include "../PC1600/PC1600LcdText.hpp"
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

// ── Synthetic ───────────────────────────────────────────────────────────

LcdFont tinyFont() {
    LcdFont f;
    f.cellHeight = 8;
    f.glyphs = {{'A', {0x7C, 0x12, 0x11, 0x12, 0x7C, 0}},
                {'\\', {0x02, 0x04, 0x08, 0x10, 0x20, 0}},
                {0x80, {0x55, 0x2A, 0x55, 0x2A, 0x55, 0}},
                {'B', {0x7C, 0x12, 0x11, 0x12, 0x7C, 0}}}; // duplicate cell: 'A' wins
    f.cursorShapes = {{0x40, 0x40, 0x40, 0x40, 0x40, 0}};
    return f;
}

LcdBitmap blankBitmap(int cols, int rows) {
    LcdBitmap b;
    b.cols = cols;
    b.rows = rows;
    b.pixels.assign(static_cast<std::size_t>(cols) * rows, false);
    return b;
}

void put(LcdBitmap& b, int textRow, int textCol, const LcdCell& cell, bool invert = false) {
    for (int c = 0; c < 6; ++c)
        for (int r = 0; r < 8; ++r)
            b.pixels[static_cast<std::size_t>(textRow * 8 + r) * b.cols + textCol * 6 + c] =
                (((cell[c] >> r) & 1) != 0) != invert;
}

void test_synthetic_cells() {
    const LcdFont f = tinyFont();
    LcdBitmap b = blankBitmap(156, 16);
    put(b, 0, 0, f.glyphs[0].second);
    put(b, 0, 1, f.glyphs[1].second);
    put(b, 0, 2, f.glyphs[2].second);
    put(b, 0, 4, f.glyphs[0].second, /*invert=*/true);
    put(b, 1, 3, {0x01, 0, 0, 0, 0, 0}); // one stray dot: not text
    const LcdText t = parseLcdText(b, f);
    CHECK(t.rows.size() == 2);
    CHECK(t.rows[0] == "A\\\\\\x80 A");
    CHECK(t.reverseCells == 1);
    CHECK(t.rows[1] == "   \xEF\xBF\xBD");
    CHECK(t.unparsed == 1);
    CHECK(t.cursorRow == -1);
    CHECK(t.contains("\\x80"));
    CHECK(!t.contains("A\n"));
}

void test_synthetic_cursor() {
    const LcdFont f = tinyFont();
    LcdBitmap b = blankBitmap(156, 8);
    put(b, 0, 0, f.glyphs[0].second);
    put(b, 0, 1, f.cursorShapes[0]);
    // No cursor reported: the shape is not a glyph.
    LcdText t = parseLcdText(b, f);
    CHECK(t.rows[0] == "A\xEF\xBF\xBD" && t.unparsed == 1 && t.cursorRow == -1);
    // Cursor over an 'A' the ROM saved, and over a blank cell; position
    // unknown (first cursor-shaped cell) and known.
    LcdCursor overA;
    overA.under = f.glyphs[0].second;
    t = parseLcdText(b, f, overA);
    CHECK(t.rows[0] == "AA" && t.unparsed == 0 && t.cursorRow == 0 && t.cursorCol == 1);
    t = parseLcdText(b, f, LcdCursor{});
    CHECK(t.rows[0] == "A" && t.cursorCol == 1);
    // A known position elsewhere: the cursor-shaped cell stays unparsed,
    // and the cursor is reported where the model said.
    LcdCursor elsewhere = overA;
    elsewhere.row = 0;
    elsewhere.col = 5;
    t = parseLcdText(b, f, elsewhere);
    CHECK(t.unparsed == 1 && t.cursorRow == 0 && t.cursorCol == 5);
}

void test_powered_off() {
    LcdBitmap b = blankBitmap(156, 8);
    put(b, 0, 0, tinyFont().glyphs[0].second);
    b.poweredOn = false;
    const LcdText t = parseLcdText(b, tinyFont());
    CHECK(!t.poweredOn && t.rows.size() == 1 && t.rows[0].empty());
    CHECK(t.report() == "(display off)\nstatus:\n");
}

// Prints every printable code 20H-7EH, 26 per line (one LCD row): line
// 10*k builds batch k in B$(0) -- FOR/NEXT is not allowed in direct mode,
// and a plain string variable holds only 16 characters.
std::string roundtripProgram() {
    std::string program;
    for (int k = 1, from = 0x20; from <= 0x7E; ++k, from += 26) {
        const int to = from + 25 > 0x7E ? 0x7E : from + 25;
        program += std::to_string(k * 10) + " DIM B$(0)*26:FOR I=" + std::to_string(from) + " TO " +
                   std::to_string(to) + ":B$(0)=B$(0)+CHR$(I):NEXT I:PRINT B$(0):END\n";
    }
    return program;
}

// What batch `batch` reads back as: each code's cell decodes to the first
// code with the same cell (the PC-1500 font has no backtick -- 60H is
// blank, like the space -- and the PC-1600's FFH is blank too).
std::string roundtripWant(const LcdFont& font, int batch) {
    const int from = 0x20 + 26 * batch, to = from + 25 > 0x7E ? 0x7E : from + 25;
    std::string want;
    for (int c = from; c <= to; ++c) {
        LcdCell cell{};
        for (const auto& [code, g] : font.glyphs)
            if (code == c) {
                cell = g; // the main glyph, not a CGSPEC alternate
                break;
            }
        int shown = c;
        for (auto it = font.glyphs.rbegin(); it != font.glyphs.rend(); ++it)
            if (it->second == cell) shown = it->first;
        if (cell == LcdCell{}) shown = ' ';
        if (shown == '\\') want += "\\\\";
        else want += static_cast<char>(shown);
    }
    return want;
}

constexpr int kRoundtripBatches = 4;

// ── PC-1600, real ROM ───────────────────────────────────────────────────

const uint64_t kPC1600Settle = PC1600Machine::kTStateHz * 10;

LcdText pc1600Run(PC1600Machine& m, const std::string& line) {
    std::string err;
    typeLine(m, line, /*pressEnter=*/true, &err);
    waitUntilBasicIdle(m, kPC1600Settle);
    return pc1600LcdText(m);
}

void test_pc1600_font_from_both_roms() {
    for (const char* version : {"new", "old"}) {
        PC1600Machine m;
        std::string error;
        if (!BundledRoms::loadPC1600RomSet(m, {"roms"}, version, &error)) {
            std::fprintf(stderr, "SKIP test_pc1600_font_from_both_roms (%s): %s\n", version, error.c_str());
            continue;
        }
        const LcdFont f = pc1600LcdFont(m);
        CHECK(f.glyphs.size() == 0x60 + 0x80 + 4);
        // 'A' at 41H, as dumped from both ROMs.
        CHECK(f.glyphs.size() > 0x21 && f.glyphs[0x21].first == 'A' &&
              f.glyphs[0x21].second == (LcdCell{0x7C, 0x12, 0x11, 0x12, 0x7C, 0x00}));
    }
}

void test_pc1600_print_roundtrip() {
    PC1600Machine m;
    if (!bootPC1600(m)) {
        std::fprintf(stderr, "SKIP test_pc1600_print_roundtrip: PC-1600 ROMs not found\n");
        return;
    }
    LcdText t = pc1600Run(m, "PRINT MEM");
    CHECK(t.contains("11834"));
    CHECK(t.unparsed == 0);
    tapKey(m, "mode"); // PRO, to type the program
    waitIdle(m, kPC1600Settle);
    CHECK(typeBasicProgramText(m, roundtripProgram()).rejectedLines.empty());
    tapKey(m, "mode");
    waitForKeyboardScanLoop(m); // MODE redraws the screen; keys typed before are lost
    const LcdFont font = pc1600LcdFont(m);
    for (int k = 0; k < kRoundtripBatches; ++k) {
        t = pc1600Run(m, "RUN " + std::to_string((k + 1) * 10));
        const std::string want = roundtripWant(font, k);
        CHECK(t.rows.size() == 4);
        CHECK(t.contains(want));
        if (!t.contains(want)) std::fprintf(stderr, "  want \"%s\", screen:\n%s", want.c_str(), t.report().c_str());
        CHECK(t.unparsed == 0);
    }
    CHECK(std::find(t.status.begin(), t.status.end(), std::string("RUN")) != t.status.end());
}

void test_pc1600_graphics_is_unparsed() {
    PC1600Machine m;
    if (!bootPC1600(m)) {
        std::fprintf(stderr, "SKIP test_pc1600_graphics_is_unparsed: PC-1600 ROMs not found\n");
        return;
    }
    const LcdText t = pc1600Run(m, "LINE (0,3)-(155,28)");
    CHECK(t.unparsed > 0);
}

// ── PC-1500, real ROM ───────────────────────────────────────────────────

bool bootPC1500(PC1500Machine& m, const char* rom) {
    if (!m.loadROMFile(rom)) return false;
    m.reset();
    m.runCycles(static_cast<uint64_t>(PC1500Machine::kCpuHz * 2));
    waitIdle(m, static_cast<uint64_t>(PC1500Machine::kCpuHz * 5));
    tapKey(m, "cl"); // the cold boot's "NEW0? :CHECK", as the preset loader does
    waitIdle(m, static_cast<uint64_t>(PC1500Machine::kCpuHz * 2));
    return true;
}

LcdText pc1500Run(PC1500Machine& m, const std::string& line) {
    std::string err;
    typeLine(m, line, /*pressEnter=*/true, &err);
    waitUntilBasicIdle(m, static_cast<uint64_t>(PC1500Machine::kCpuHz * 10));
    return pc1500LcdText(m);
}

void test_pc1500_print_roundtrip() {
    for (const char* rom : {"roms/PC-1500_A04.ROM", "roms/PC-1500_A03.ROM"}) {
        PC1500Machine m;
        if (!bootPC1500(m, rom)) {
            std::fprintf(stderr, "SKIP test_pc1500_print_roundtrip: %s not found\n", rom);
            continue;
        }
        tapKey(m, "mode"); // boots in PRO
        waitIdle(m, static_cast<uint64_t>(PC1500Machine::kCpuHz * 2));
        LcdText t = pc1500Run(m, "PRINT 12345*-2");
        CHECK(t.rows.size() == 1);
        CHECK(t.contains("-24690"));
        CHECK(t.unparsed == 0);
        // Each batch in direct mode, in A$-D$ (7 codes each: a string
        // variable holds 16 characters, a line 80).
        const LcdFont font = pc1500LcdFont(m);
        for (int k = 0; k < kRoundtripBatches; ++k) {
            const int from = 0x20 + 26 * k, to = from + 25 > 0x7E ? 0x7E : from + 25;
            for (int v = 0; v < 4; ++v) {
                std::string line = std::string(1, char('A' + v)) + "$=\"\"";
                for (int c = from + 7 * v; c <= to && c < from + 7 * (v + 1); ++c) line += "+CHR$ " + std::to_string(c);
                pc1500Run(m, line);
            }
            t = pc1500Run(m, "PRINT A$;B$;C$;D$");
            const std::string want = roundtripWant(font, k);
            CHECK(t.contains(want));
            if (!t.contains(want)) std::fprintf(stderr, "  %s want \"%s\", screen:\n%s", rom, want.c_str(), t.report().c_str());
            CHECK(t.unparsed == 0);
        }
        CHECK(std::find(t.status.begin(), t.status.end(), std::string("RUN")) != t.status.end());
    }
}

// ── `expect:` preset step ───────────────────────────────────────────────

void test_expect_parse() {
    PresetFile p;
    std::string err;
    CHECK(parsePresetString("model: PC-1600\nkeys:\n  - expect: ERROR 1 # not a comment\n",
                            "/tmp/lcd_text_tests_scratch.pc1600", &p, &err));
    CHECK(!p.sections.empty() && !p.sections[0].keys.empty() &&
          p.sections[0].keys[0].kind == PresetStep::Kind::Expect &&
          p.sections[0].keys[0].text == "ERROR 1 # not a comment");
    PresetFile bad;
    CHECK(!parsePresetString("model: PC-1600\nkeys:\n  - expect:\n", "/tmp/lcd_text_tests_scratch.pc1600", &bad,
                             &err));
}

// MEM with and without the CE-1600P: the floppy module's work area (428H
// bytes, EXROMWK) is reserved as soon as the drive answers -- and the
// emulator attaches a CE-1600F with every CE-1600P.
void test_expect_pc1600() {
    struct Case {
        const char* text;
        bool ok;
    };
    const Case cases[] = {
        {"model: PC-1600\nkeys:\n  - type: PRINT MEM\n  - expect: 11834\n", true},
        {"model: PC-1600\nplotter: CE-1600P\nkeys:\n  - type: PRINT MEM\n  - expect: 10810\n", true},
        {"model: PC-1600\nkeys:\n  - type: PRINT MEM\n  - expect: 10810\n", false},
    };
    for (const Case& c : cases) {
        PresetFile p;
        std::string err;
        CHECK(parsePresetString(c.text, "/tmp/lcd_text_tests_scratch.pc1600", &p, &err));
        PC1600Machine m;
        if (!loadPC1600Roms(m)) { // the preset loader boots whatever ROMs are loaded
            std::fprintf(stderr, "SKIP test_expect_pc1600: PC-1600 ROMs not found\n");
            return;
        }
        const PresetLoadResult r = applyPC1600Preset(m, p, {}, "/tmp", ".", {}, {"roms"});
        if (!r.ok && r.error.find("ROM") != std::string::npos && r.error.find("expect") == std::string::npos) {
            std::fprintf(stderr, "SKIP test_expect_pc1600: %s\n", r.error.c_str());
            return;
        }
        CHECK(r.ok == c.ok);
        if (!c.ok) CHECK(r.error.find("11834") != std::string::npos); // the failure shows the screen
        if (r.ok != c.ok) std::fprintf(stderr, "  %s-> %s\n", c.text, r.error.c_str());
    }
}

void test_expect_pc1500() {
    PresetFile p;
    std::string err;
    CHECK(parsePresetString("model: PC-1500\nkeys:\n  - key: mode\n  - type: PRINT 12345*-2\n  - expect: -24690\n",
                            "/tmp/lcd_text_tests_scratch.pc1500", &p, &err));
    PC1500Machine m;
    const PresetLoadResult r = applyPC1500Preset(m, p, {}, "/tmp", ".", {}, {"roms"});
    if (!r.ok && r.error.find("ROM") != std::string::npos && r.error.find("expect") == std::string::npos) {
        std::fprintf(stderr, "SKIP test_expect_pc1500: %s\n", r.error.c_str());
        return;
    }
    CHECK(r.ok);
    if (!r.ok) std::fprintf(stderr, "  %s\n", r.error.c_str());
}

void test_log_field() {
    LcdText t;
    t.rows = {"A\"B", ""};
    CHECK(t.logField() == "lcd=[\"A\\\"B\",\"\"]");
    t.poweredOn = false;
    CHECK(t.logField() == "lcd=off");
}

} // namespace

int run_lcd_text_tests() {
    test_synthetic_cells();
    test_synthetic_cursor();
    test_powered_off();
    test_pc1600_font_from_both_roms();
    test_pc1600_print_roundtrip();
    test_pc1600_graphics_is_unparsed();
    test_pc1500_print_roundtrip();
    test_expect_parse();
    test_expect_pc1600();
    test_expect_pc1500();
    test_log_field();

    std::printf("lcd_text_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
