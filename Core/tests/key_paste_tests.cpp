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
        if (s.kind == PasteStep::Kind::Enter) { out += "<enter>"; continue; }
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

void test_build_steps_line_breaks() {
    // CRLF and lone CR are one line break; the single trailing one is dropped.
    CHECK(tapsAsText(buildPasteSteps("A\r\nB\rC\n", pc1500ResolveTypedChar)) == "A <enter>B <enter>C ");
    CHECK(tapsAsText(buildPasteSteps("A\n\n", pc1500ResolveTypedChar)) == "A <enter>");
    CHECK(buildPasteSteps("\n", pc1500ResolveTypedChar).empty());
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
    for (r.frame = 0; r.frame < 100 && f.active(); ++r.frame) f.onFrame(r.press(), r.release(), false);
    // shift: 4 held + 4 gap, 6-frame shift gap, A: 4+4, then 1: 4+4.
    const std::vector<std::string> expected = {"0+shift", "4-shift", "14+A", "18-A", "22+1", "26-1"};
    CHECK(r.events == expected);
    CHECK(!f.active());
}

void test_feeder_waits_for_prompt_after_enter() {
    KeyPasteFeeder f;
    f.setPacing(pc1500PastePacing());
    f.append(buildPasteSteps("1\n2", pc1500ResolveTypedChar));
    Recorder r;
    // Prompt only reached from frame 40 on: "2" must wait for 20 at-prompt frames.
    for (r.frame = 0; r.frame < 400 && f.active(); ++r.frame) f.onFrame(r.press(), r.release(), r.frame >= 40);
    CHECK(r.events.size() == 6);
    CHECK(r.events.size() == 6 && r.events[2] == "8+enter");
    CHECK(r.events.size() == 6 && r.events[4] == "59+2");
}

void test_feeder_prompt_wait_is_capped() {
    KeyPasteFeeder f;
    f.append(buildPasteSteps("1\n2", pc1500ResolveTypedChar));
    Recorder r;
    for (r.frame = 0; r.frame < 1000 && f.active(); ++r.frame) f.onFrame(r.press(), r.release(), false);
    CHECK(!f.active()); // gave up waiting (~5 s) and typed "2" anyway
    CHECK(!r.events.empty() && r.events.back().find("-2") != std::string::npos);
}

void test_feeder_cancel_releases_held_key() {
    KeyPasteFeeder f;
    f.append(buildPasteSteps("AB", pc1500ResolveTypedChar));
    Recorder r;
    f.onFrame(r.press(), r.release(), false); // A pressed
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
template <typename Machine, typename AtPrompt>
int pasteInto(Machine& m, uint64_t frameCycles, const PastePacing& pacing, TypedCharResolver resolve,
              const std::string& text, AtPrompt atPrompt) {
    KeyPasteFeeder f;
    f.setPacing(pacing);
    f.append(buildPasteSteps(text, resolve));
    int enters = 0;
    auto press = [&](const std::string& k) { if (k == "enter") ++enters; m.pressKey(k); };
    auto release = [&](const std::string& k) { m.releaseKey(k); };
    for (int frame = 0; frame < 60 * 120 && f.active(); ++frame) {
        m.runCycles(frameCycles);
        f.onFrame(press, release, atPrompt(m));
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
                                 line, [](const PC1600Machine& mm) { return pc1600AtBasicPrompt(mm); });
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

// Interior line breaks are ENTERs: in PRO mode both lines get stored
// (BASPRG_END, F867H big-endian, grows), the last one only once ENTER is
// tapped by hand -- the trailing-line rule.
void test_pc1600_paste_multiline_program() {
    PC1600Machine m;
    if (!bootPC1600(m)) {
        std::fprintf(stderr, "SKIP test_pc1600_paste_multiline_program: PC-1600 ROM images not found\n");
        return;
    }
    tapKey(m, "mode"); // RUN -> PRO
    m.runCycles(PC1600Machine::kTStateHz / 2);
    const uint16_t before = readBE16(m, 0xF867);
    const int enters = pasteInto(m, PC1600Machine::kTStateHz / 60, pc1600PastePacing(), pc1600ResolveTypedChar,
                                 "10 PRINT \"Hi\"\n20 A=1\n30 END",
                                 [](const PC1600Machine& mm) { return pc1600AtBasicPrompt(mm); });
    CHECK(enters == 2);
    m.runCycles(PC1600Machine::kTStateHz / 2);
    const uint16_t afterTwo = readBE16(m, 0xF867);
    CHECK(afterTwo > before);
    tapKey(m, "enter");
    m.runCycles(PC1600Machine::kTStateHz / 2);
    CHECK(readBE16(m, 0xF867) > afterTwo);
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

void test_pc1500_paste_multiline_program() {
    PC1500Machine m;
    if (!bootPC1500(m)) {
        std::fprintf(stderr, "SKIP test_pc1500_paste_multiline_program: roms/PC-1500_A04.ROM not found\n");
        return;
    }
    // A cold-booted PC-1500 needs CL + NEW0 before it stores program lines
    // (typeBasicProgramText()'s own preamble).
    tapKey(m, "cl");
    waitIdle(m, static_cast<uint64_t>(1300000.0 * 2));
    std::string err;
    CHECK(typeLine(m, "NEW0", /*pressEnter=*/true, &err));
    const int enters = pasteInto(m, static_cast<uint64_t>(1300000.0 / 60), pc1500PastePacing(),
                                 pc1500ResolveTypedChar, "10 PRINT \"Paste: ok\"\n20 REM Tail#x",
                                 [](const PC1500Machine& mm) { return pc1500AtBasicPrompt(mm); });
    CHECK(enters == 1);
    CHECK(memoryContains(m, "Paste: ok")); // case kept via the SHIFT taps
    // No ENTER yet for the last line: BASPRG_END ($7867, big-endian) only
    // moves once it is committed. (Its text already sits in the line
    // buffer, so a plain memory search can't tell.)
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
    test_build_steps_line_breaks();
    test_build_steps_pc1500_lacks_digit_row_legends();
    test_feeder_cadence();
    test_feeder_waits_for_prompt_after_enter();
    test_feeder_prompt_wait_is_capped();
    test_feeder_cancel_releases_held_key();
    test_pc1600_paste_line_no_enter();
    test_pc1600_paste_multiline_program();
    test_pc1500_paste_multiline_program();

    std::printf("key_paste_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
