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
#include <utility>
#include <vector>

#include "../CPU/LH5803/LH5803SharedMemory.hpp"
#include "../Display/LcdText.hpp"
#include "../PC1500/PC1500BasicTyper.hpp"
#include "../PC1500/PC1500Display.hpp"
#include "../PC1500/PC1500Memory.hpp"
#include "../PC1500/PC1500LcdText.hpp"
#include "../PC1500/PC1500PresetLoader.hpp"
#include "../PC1600/PC1600Bank.hpp"
#include "../PC1600/PC1600BasicTyper.hpp"
#include "../PC1600/PC1600Display.hpp"
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

// ── Status line: one vocabulary for both models ─────────────────────────

// The same two bytes (PC-1500 764EH/764FH, PC-1600 IC3 column 63 pages
// 7/6) read as the same words on both displays.
void test_status_words_same_on_both_models() {
    struct Case {
        uint8_t set0, set1;
        std::vector<std::string> want;
    };
    const Case cases[] = {
        {0x40, 0x43, {"DEG", "RUN", "I"}},
        {0x40, 0x46, {"GRAD", "RUN", "I"}},
        {0x04, 0x24, {"KANA", "RAD", "PRO"}},
        {0xBB, 0x10, {"BUSY", "SHIFT", "SMALL", "RESERVE", "DEF", "II", "III"}},
    };
    for (const Case& c : cases) {
        PC1500Memory mem;
        mem.poke(0x764E, c.set0);
        mem.poke(0x764F, c.set1);
        const std::vector<std::string> pc1500 = statusWords(PC1500Display(mem).statusLine());

        PC1600Display d;
        d.writeIO(0x54, 0x3F); // IC3 display on
        for (const auto& [page, value] : {std::pair<int, uint8_t>{7, c.set0}, {6, c.set1}}) {
            d.writeIO(0x54, uint8_t(0xB8 | page));
            d.writeIO(0x54, 0x40 | 63);
            d.writeIO(0x56, value);
        }
        const std::vector<std::string> pc1600 = statusWords(d.statusLine());
        CHECK(pc1500 == c.want);
        CHECK(pc1600 == c.want);
    }
}

// Both machines boot with DEG and I lit (and RUN or PRO, by boot path).
void test_status_after_boot() {
    PC1600Machine m16;
    if (bootPC1600(m16)) {
        const LcdText t = pc1600LcdText(m16);
        CHECK(t.status.size() >= 3 && t.status[0] == "DEG" && t.status.back() == "I");
    } else {
        std::fprintf(stderr, "SKIP test_status_after_boot (PC-1600): ROMs not found\n");
    }
    PC1500Machine m15;
    if (bootPC1500(m15, "roms/PC-1500_A04.ROM")) {
        const LcdText t = pc1500LcdText(m15);
        CHECK(t.status.size() >= 3 && t.status[0] == "DEG" && t.status.back() == "I");
    } else {
        std::fprintf(stderr, "SKIP test_status_after_boot (PC-1500): roms/PC-1500_A04.ROM not found\n");
    }
}

// GRAD / RADIAN on the PC-1500 write 764FH, the glass itself.
void test_status_angle_pc1500() {
    PC1500Machine m;
    if (!bootPC1500(m, "roms/PC-1500_A04.ROM")) {
        std::fprintf(stderr, "SKIP test_status_angle_pc1500: roms/PC-1500_A04.ROM not found\n");
        return;
    }
    tapKey(m, "mode");
    waitIdle(m, static_cast<uint64_t>(PC1500Machine::kCpuHz * 2));
    LcdText t = pc1500Run(m, "GRAD");
    CHECK(std::find(t.status.begin(), t.status.end(), std::string("GRAD")) != t.status.end());
    t = pc1500Run(m, "RADIAN");
    CHECK(std::find(t.status.begin(), t.status.end(), std::string("RAD")) != t.status.end());
    t = pc1500Run(m, "DEGREE");
    CHECK(std::find(t.status.begin(), t.status.end(), std::string("DEG")) != t.status.end());
}

// ── PC-1600: the gate array mirrors LH5803 writes to the PC-1500 display ─
// RAM onto the LCD (measured on a real PC-1600, 2026-10-03).

// Column `x`'s dots on text row `row` (bit n = dot n).
uint8_t lcdColumn(const LcdBitmap& b, int row, int x) {
    uint8_t dots = 0;
    for (int dot = 0; dot < 8; ++dot)
        if (b.pixels[static_cast<std::size_t>(row * 8 + dot) * b.cols + x]) dots |= uint8_t(1u << dot);
    return dots;
}

void test_pc1600_mirror_unit() {
    PC1600Display d;
    d.writeIO(0x50, 0x3F); // both controllers on
    for (int startLine : {0, 8, 40}) {
        d.writeIO(0x50, uint8_t(0xC0 | startLine));
        for (int col : {0, 39, 63, 64, 100, 127, 128, 155}) {
            d.mirrorPc1500Column(col, 0xA5);
            uint8_t dots = 0;
            for (int dot = 0; dot < 8; ++dot)
                if (d.pixel(col, 24 + dot)) dots |= uint8_t(1u << dot);
            CHECK(dots == 0xA5); // always the visible bottom line
            d.mirrorPc1500Column(col, 0x00);
            CHECK(!d.pixel(col, 24) && !d.pixel(col, 31)); // replaces, not ORs
        }
        d.mirrorPc1500StatusSet(1, 0x46);
        CHECK(statusWords(d.statusLine()) == (std::vector<std::string>{"GRAD", "RUN"}));
    }
}

void test_pc1600_mirror_real_rom() {
    PC1600Machine m;
    if (!bootPC1600(m)) {
        std::fprintf(stderr, "SKIP test_pc1600_mirror_real_rom: PC-1600 ROMs not found\n");
        return;
    }
    // DEGREE/GRAD run on the LH5803 (CD7DH) and store 764FH: the legend
    // changes at once, no scroll needed.
    pc1600Run(m, "CLS");
    LcdText t = pc1600Run(m, "GRAD");
    CHECK(t.status.size() >= 1 && t.status[0] == "GRAD");
    t = pc1600Run(m, "DEGREE");
    CHECK(t.status.size() >= 1 && t.status[0] == "DEG");

    // The hardware test: 7601H = 80H left over in RAM, then on a clear
    // screen 7700H = 7FH and 7600H = 01H. Bottom line: column 0 dot 0,
    // 39 dots 0-3, 78 dot 7 (from the old 7601H), 117 dots 0-2.
    pc1600Run(m, "XPOKE &7601,&80");
    pc1600Run(m, "CLS");
    pc1600Run(m, "XPOKE &7700,&7F");
    pc1600Run(m, "XPOKE &7600,&01");
    const LcdBitmap b = pc1600LcdBitmap(m);
    CHECK(lcdColumn(b, 3, 0) == 0x01);
    CHECK(lcdColumn(b, 3, 39) == 0x0F);
    CHECK(lcdColumn(b, 3, 78) == 0x80);
    CHECK(lcdColumn(b, 3, 117) == 0x07);
    CHECK(lcdColumn(b, 3, 1) == 0x00 && lcdColumn(b, 3, 79) == 0x00);

    // After the screen has scrolled the dots still land on the bottom line
    // -- and the ENTER of the XPOKE line itself scrolls them up one.
    for (int i = 0; i < 6; ++i) pc1600Run(m, "PRINT " + std::to_string(i));
    pc1600Run(m, "XPOKE &7600,&7F");
    const LcdBitmap scrolled = pc1600LcdBitmap(m);
    CHECK(lcdColumn(scrolled, 2, 0) == 0x0F);
    CHECK(lcdColumn(scrolled, 2, 78) == 0x87); // dots 0-2 and the old 7601H's dot 7
}

// ME0 writes drive the mirror; ME1 writes reach only the RAM.
void test_pc1600_mirror_me0_only() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    LH5803SharedMemory lh(mem);
    mem.display().writeIO(0x50, 0x3F); // both controllers on
    lh.writeME0(0x764F, 0x46);
    CHECK(statusWords(mem.display().statusLine()) == (std::vector<std::string>{"GRAD", "RUN"}));
    lh.writeME1(0x764F, 0x44);
    CHECK(mem.read(0xF64F) == 0x44);
    CHECK(statusWords(mem.display().statusLine()) == (std::vector<std::string>{"GRAD", "RUN"}));
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
    test_status_words_same_on_both_models();
    test_status_after_boot();
    test_status_angle_pc1500();
    test_pc1600_mirror_unit();
    test_pc1600_mirror_real_rom();
    test_pc1600_mirror_me0_only();

    std::printf("lcd_text_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
