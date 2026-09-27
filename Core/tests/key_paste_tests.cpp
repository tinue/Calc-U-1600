// Headless C++ tests for Core/KeyPaste (the GUI's Edit > Paste Text
// feeder): step building, the frame-paced state machine, and -- ROM-gated
// -- pasted text actually reaching each model's ROM without a lost
// character. ROM-gated tests SKIP (not fail) when the images are absent.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../KeyPaste.hpp"
#include "../PC1500/PC1500BasicTyper.hpp"
#include "../PC1500/PC1500Machine.hpp"
#include "../PC1500/PC1500TypedInput.hpp"
#include "../PC1600/PC1600BasicTyper.hpp"
#include "../PC1600/PC1600Machine.hpp"
#include "../PC1600/PC1600TypedInput.hpp"
#include "TestRoms.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

std::string tapsAsText(const std::vector<PasteStep>& steps) {
    std::string out;
    for (const PasteStep& s : steps) {
        if (s.needsKbii) out += "~";
        if (s.needsShift) out += "^";
        out += s.key == "space" ? std::string("_") : s.key;
        out += ' ';
    }
    return out;
}

void test_build_steps_shift_and_skip() {
    // Lowercase + '"' + ':' + '#' need SHIFT; tab and a UTF-8 'é' are skipped.
    const std::vector<PasteStep> steps = buildPasteSteps("aB\"\t:#\xC3\xA9 1", pc1600ResolveTypedChar);
    CHECK(tapsAsText(steps) == "^A B ^f2 ^* ^f3 _ 1 ");
}

void test_build_steps_kbii_case_exact() {
    // é/É: SHIFT+KBII S / KBII S. Ë has no uppercase in the ROM -> ë (W).
    // û only exists as SHIFT+KBII. ß has no key and is skipped.
    const std::string text = "m\xC3\xA9rci \xC3\x89\xC3\x8B\xC3\xAB\xC3\xBB\xC3\x9B\xC3\x9F";
    CHECK(tapsAsText(buildPasteSteps(text, pc1600ResolveTypedChar, pc1600ResolveTypedKbiiChar)) ==
          "^M ~^S ^R ^C ^I _ ~S ~W ~W ~^B ~^B ");
    // Without a KBII resolver (the PC-1500) non-ASCII is skipped.
    CHECK(tapsAsText(buildPasteSteps(text, pc1500ResolveTypedChar)) == "^M ^R ^C ^I _ ");
    // A malformed byte is skipped, the rest still typed.
    CHECK(tapsAsText(buildPasteSteps("A\xC3" "B\xFF", pc1600ResolveTypedChar, pc1600ResolveTypedKbiiChar)) == "A B ");
}

void test_kbii_resolver() {
    // One rule everywhere: the character as typed; an uppercase the ROM
    // lacks falls back to its lowercase.
    std::string key;
    bool shift = true;
    CHECK(pc1600ResolveKbiiChar(U'ö', &key, &shift) && key == "R" && shift);   // ö
    CHECK(pc1600ResolveKbiiChar(U'Ö', &key, &shift) && key == "R" && !shift);  // Ö
    CHECK(pc1600ResolveKbiiChar(U'ë', &key, &shift) && key == "W" && !shift);  // ë
    CHECK(pc1600ResolveKbiiChar(U'Ë', &key, &shift) && key == "W" && !shift);  // Ë
    CHECK(pc1600ResolveKbiiChar(U'Û', &key, &shift) && key == "B" && shift);   // Û -> û
    CHECK(pc1600ResolveKbiiChar(U'«', &key, &shift) && key == "(" && shift);   // «
    CHECK(!pc1600ResolveKbiiChar(U'ß', &key, &shift));                        // ß
}

void test_build_steps_line_breaks() {
    // Only the first line is typed; ENTER is never pressed.
    CHECK(tapsAsText(buildPasteSteps("A1\r\nB\rC\n", pc1500ResolveTypedChar)) == "A 1 ");
    CHECK(tapsAsText(buildPasteSteps("A\rB", pc1500ResolveTypedChar)) == "A ");
    CHECK(tapsAsText(buildPasteSteps("A\n\n", pc1500ResolveTypedChar)) == "A ");
    CHECK(buildPasteSteps("\nB", pc1500ResolveTypedChar).empty());
}

void test_build_steps_pc1500_lacks_digit_row_legends() {
    // '[' only exists on the PC-1600 keyboard (SHIFT + 2).
    CHECK(tapsAsText(buildPasteSteps("[", pc1500ResolveTypedChar)).empty());
    CHECK(tapsAsText(buildPasteSteps("[", pc1600ResolveTypedChar)) == "^2 ");
}

struct Recorder {
    std::vector<std::string> events; // "+key" / "-key", tagged with the frame
    int frame = 0;
    KeyPasteFeeder::KeyFn press() {
        return [this](const std::string& k) { events.push_back(std::to_string(frame) + "+" + k); };
    }
    KeyPasteFeeder::KeyFn release() {
        return [this](const std::string& k) { events.push_back(std::to_string(frame) + "-" + k); };
    }
};

void test_feeder_cadence() {
    KeyPasteFeeder f;
    f.setPacing(pc1600PastePacing());
    f.append(buildPasteSteps("a1", pc1600ResolveTypedChar));
    Recorder r;
    for (r.frame = 0; r.frame < 100 && f.active(); ++r.frame) f.onFrame(r.press(), r.release());
    // shift: 4 held + 4 gap, 6-frame shift gap, A: 4+4, then 1: 4+4.
    const std::vector<std::string> expected = {"0+shift", "4-shift", "14+A", "18-A", "22+1", "26-1"};
    CHECK(r.events == expected);
    CHECK(!f.active());
}

void test_feeder_kbii_sequence() {
    KeyPasteFeeder f;
    f.setPacing(pc1600PastePacing());
    f.append(buildPasteSteps("\xC3\xA9", pc1600ResolveTypedChar, pc1600ResolveTypedKbiiChar)); // é
    Recorder r;
    for (r.frame = 0; r.frame < 200 && f.active(); ++r.frame) f.onFrame(r.press(), r.release());
    // kbii 4+4 +6, shift 4+4 +6, S 4+4, kbii 4+4 +6.
    const std::vector<std::string> expected = {"0+kbii", "4-kbii", "14+shift", "18-shift",
                                               "28+S",   "32-S",   "36+kbii",  "40-kbii"};
    CHECK(r.events == expected);
}

// What the user has latched on the machine shapes (or drops) an accented
// character's sequence -- read when it starts, not when it's queued.
void test_feeder_kbii_respects_latches() {
    auto run = [](const KeyLatches& latched) {
        KeyPasteFeeder f;
        f.setPacing(pc1600PastePacing());
        f.append(buildPasteSteps("\xC3\x96" "A", pc1600ResolveTypedChar, pc1600ResolveTypedKbiiChar)); // Ö A
        std::string keys;
        auto press = [&keys](const std::string& k) { keys += k + " "; };
        auto release = [](const std::string&) {};
        bool first = true;
        auto latches = [&] {
            const KeyLatches l = first ? latched : KeyLatches{};
            first = false;
            return l;
        };
        for (int frame = 0; frame < 200 && f.active(); ++frame) f.onFrame(press, release, latches);
        return keys;
    };
    CHECK(run(KeyLatches{}) == "kbii R kbii A ");
    CHECK(run(KeyLatches{true, false}) == "shift kbii R kbii A ");  // SHIFT un-latched first
    CHECK(run(KeyLatches{false, true}) == "A ");                    // KBII on: dropped
    CHECK(run(KeyLatches{true, true}) == "A ");
}

void test_feeder_cancel_closes_kbii() {
    const std::string text = "\xC3\x84" "B"; // Ä then B
    // Cancelled while the letter is held: release it, then only the closing KBII tap.
    {
        KeyPasteFeeder f;
        f.setPacing(pc1600PastePacing());
        f.append(buildPasteSteps(text, pc1600ResolveTypedChar, pc1600ResolveTypedKbiiChar));
        Recorder r;
        for (r.frame = 0; r.frame <= 14; ++r.frame) f.onFrame(r.press(), r.release()); // Q pressed at 14, cancel at 15
        f.cancel(r.release(), /*finishKbii=*/true);
        for (; r.frame < 100 && f.active(); ++r.frame) f.onFrame(r.press(), r.release());
        const std::vector<std::string> expected = {"0+kbii", "4-kbii", "14+Q", "15-Q", "15+kbii", "19-kbii"};
        CHECK(r.events == expected);
    }
    // Cancelled while the opening KBII is still held: it runs to the end, then closes.
    {
        KeyPasteFeeder f;
        f.setPacing(pc1600PastePacing());
        f.append(buildPasteSteps(text, pc1600ResolveTypedChar, pc1600ResolveTypedKbiiChar));
        Recorder r;
        f.onFrame(r.press(), r.release()); // kbii pressed at frame 0
        f.cancel(r.release(), /*finishKbii=*/true);
        for (r.frame = 1; r.frame < 100 && f.active(); ++r.frame) f.onFrame(r.press(), r.release());
        const std::vector<std::string> expected = {"0+kbii", "4-kbii", "8+kbii", "12-kbii"};
        CHECK(r.events == expected);
    }
    // Cancelled after the closing tap: nothing left to do.
    {
        KeyPasteFeeder f;
        f.setPacing(pc1600PastePacing());
        f.append(buildPasteSteps(text, pc1600ResolveTypedChar, pc1600ResolveTypedKbiiChar));
        Recorder r;
        for (r.frame = 0; r.frame <= 38; ++r.frame) f.onFrame(r.press(), r.release()); // B held (36-40)
        f.cancel(r.release(), /*finishKbii=*/true);
        CHECK(!f.active());
    }
    // A reset-style cancel drops everything, KBII sequence or not.
    {
        KeyPasteFeeder f;
        f.setPacing(pc1600PastePacing());
        f.append(buildPasteSteps(text, pc1600ResolveTypedChar, pc1600ResolveTypedKbiiChar));
        Recorder r;
        for (r.frame = 0; r.frame <= 14; ++r.frame) f.onFrame(r.press(), r.release());
        f.cancel(r.release());
        CHECK(!f.active());
    }
}

void test_feeder_cancel_releases_held_key() {
    KeyPasteFeeder f;
    f.append(buildPasteSteps("AB", pc1500ResolveTypedChar));
    Recorder r;
    f.onFrame(r.press(), r.release()); // A pressed
    f.cancel(r.release());
    CHECK(!f.active());
    CHECK((r.events == std::vector<std::string>{"0+A", "0-A"}));
}

bool bootPC1500(PC1500Machine& m) {
    if (!m.loadROMFile("roms/PC-1500_A04.ROM")) return false;
    m.reset();
    m.runCycles(static_cast<uint64_t>(1300000.0 * 2));
    waitIdle(m, static_cast<uint64_t>(1300000.0 * 5));
    return true;
}

// Drives the feeder the way MachineController::advance() does: one frame
// of emulated time, then onFrame(). Returns how many ENTERs it pressed.
template <typename Machine>
int pasteInto(Machine& m, uint64_t frameCycles, const PastePacing& pacing, TypedCharResolver resolve,
              const std::string& text) {
    KeyPasteFeeder f;
    f.setPacing(pacing);
    f.append(buildPasteSteps(text, resolve));
    int enters = 0;
    auto press = [&](const std::string& k) { if (k == "enter") ++enters; m.pressKey(k); };
    auto release = [&](const std::string& k) { m.releaseKey(k); };
    for (int frame = 0; frame < 60 * 120 && f.active(); ++frame) {
        m.runCycles(frameCycles);
        f.onFrame(press, release);
    }
    return enters;
}

uint16_t readBE16(PC1600Machine& m, uint16_t addr) {
    return static_cast<uint16_t>((m.memory().peek(addr) << 8) | m.memory().peek(static_cast<uint16_t>(addr + 1)));
}

// The motivating example: a listing line pasted verbatim, no ENTER sent.
// It only reaches the console input buffer (FBB0H) on commit, so the test
// taps ENTER itself afterwards and checks the characters made it.
void test_pc1600_paste_line_no_enter() {
    PC1600Machine m;
    if (!bootPC1600(m)) {
        std::fprintf(stderr, "SKIP test_pc1600_paste_line_no_enter: PC-1600 ROM images not found\n");
        return;
    }
    const std::string line = "OPEN \"X:ATLANTIS.PUN\"FOR INPUT AS #1";
    const int enters = pasteInto(m, PC1600Machine::kTStateHz / 60, pc1600PastePacing(), pc1600ResolveTypedChar,
                                 line);
    CHECK(enters == 0);
    tapKey(m, "enter");
    waitIdle(m, PC1600Machine::kTStateHz);
    std::string buf;
    for (uint16_t a = 0xFBB0; a <= 0xFBFF; ++a) buf.push_back(static_cast<char>(m.memory().read(a)));
    // The ROM keyword-tokenises on commit (OPEN/FOR/INPUT/AS become
    // tokens), so check the literal parts between the keywords.
    CHECK(buf.find("\"X:ATLANTIS.PUN\"") != std::string::npos);
    CHECK(buf.find("#1") != std::string::npos);
}

// Only the first line is pasted, and never entered: in PRO mode nothing is
// stored (BASPRG_END, F867H big-endian, stays put) until ENTER is tapped by
// hand, and then exactly that one line.
void test_pc1600_paste_multiline_types_first_line() {
    PC1600Machine m;
    if (!bootPC1600(m)) {
        std::fprintf(stderr, "SKIP test_pc1600_paste_multiline_types_first_line: PC-1600 ROM images not found\n");
        return;
    }
    tapKey(m, "mode"); // RUN -> PRO
    m.runCycles(PC1600Machine::kTStateHz / 2);
    const uint16_t before = readBE16(m, 0xF867);
    const int enters = pasteInto(m, PC1600Machine::kTStateHz / 60, pc1600PastePacing(), pc1600ResolveTypedChar,
                                 "10 PRINT \"Hi\"\n20 A=1\n30 END");
    CHECK(enters == 0);
    m.runCycles(PC1600Machine::kTStateHz / 2);
    CHECK(readBE16(m, 0xF867) == before);
    tapKey(m, "enter");
    m.runCycles(PC1600Machine::kTStateHz / 2);
    const uint16_t afterOne = readBE16(m, 0xF867);
    CHECK(afterOne > before);
    tapKey(m, "enter"); // an empty line stores nothing more
    m.runCycles(PC1600Machine::kTStateHz / 2);
    CHECK(readBE16(m, 0xF867) == afterOne);
}

bool memoryContains(PC1500Machine& m, const std::string& needle) {
    for (uint32_t a = 0x4000; a + needle.size() <= 0x8000; ++a) {
        bool match = true;
        for (std::size_t i = 0; i < needle.size() && match; ++i) {
            match = m.memory().peek(static_cast<uint16_t>(a + i)) == static_cast<uint8_t>(needle[i]);
        }
        if (match) return true;
    }
    return false;
}

void test_pc1500_paste_multiline_types_first_line() {
    PC1500Machine m;
    if (!bootPC1500(m)) {
        std::fprintf(stderr, "SKIP test_pc1500_paste_multiline_types_first_line: roms/PC-1500_A04.ROM not found\n");
        return;
    }
    // A cold-booted PC-1500 needs CL + NEW0 before it stores program lines
    // (the preamble a preset's keys: gives typeBasicProgramText()).
    tapKey(m, "cl");
    waitIdle(m, static_cast<uint64_t>(1300000.0 * 2));
    std::string err;
    CHECK(typeLine(m, "NEW0", /*pressEnter=*/true, &err));
    const int enters = pasteInto(m, static_cast<uint64_t>(1300000.0 / 60), pc1500PastePacing(),
                                 pc1500ResolveTypedChar, "10 PRINT \"Paste: ok\"\n20 REM Tail#x");
    CHECK(enters == 0);
    CHECK(memoryContains(m, "Paste: ok")); // case kept via the SHIFT taps
    CHECK(!memoryContains(m, "Tail#x"));   // the second line is never typed
    // No ENTER: BASPRG_END ($7867, big-endian) only moves once the line is
    // committed by hand. (Its text already sits in the line buffer, so a
    // plain memory search can't tell.)
    auto programEnd = [&m] { return (m.memory().peek(0x7867) << 8) | m.memory().peek(0x7868); };
    const int endBeforeEnter = programEnd();
    m.runCycles(static_cast<uint64_t>(1300000.0 / 2));
    CHECK(programEnd() == endBeforeEnter);
    tapKey(m, "enter");
    settleUntilIdle(m, static_cast<uint64_t>(1300000.0 * 2));
    m.runCycles(static_cast<uint64_t>(1300000.0 / 2));
    CHECK(programEnd() > endBeforeEnter);
}

} // namespace

int run_key_paste_tests() {
    test_build_steps_shift_and_skip();
    test_build_steps_kbii_case_exact();
    test_kbii_resolver();
    test_build_steps_line_breaks();
    test_build_steps_pc1500_lacks_digit_row_legends();
    test_feeder_cadence();
    test_feeder_kbii_sequence();
    test_feeder_kbii_respects_latches();
    test_feeder_cancel_closes_kbii();
    test_feeder_cancel_releases_held_key();
    test_pc1600_paste_line_no_enter();
    test_pc1600_paste_multiline_types_first_line();
    test_pc1500_paste_multiline_types_first_line();

    std::printf("key_paste_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
