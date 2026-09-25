// Headless C++ tests for PC1500BasicTyper.cpp's scripted-keystroke typing.
// On real hardware a lowercase letter is typed as SHIFT + the base key, a
// one-shot latch, not the ROM's separate SML persistent-mode toggle; the
// typer must reproduce that exactly. Needs a real ROM (roms/PC-1500_A04.ROM,
// relative to the repo root) since the BASIC line editor this drives
// lives there -- skips (not fails) if it's missing, same convention as
// lh5801_tests.cpp's own test_boot_smoke_real_rom.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../Basic/BasicLineStoreCheck.hpp"
#include "../PC1500/PC1500BasicTyper.hpp"
#include "../PC1500/PC1500Machine.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// True if `needle`'s bytes appear contiguously anywhere in
// [start, end] -- avoids hardcoding the exact program-storage address
// (which shifts with RAM shape/tokenization), matching how this
// project's other tests scan for content rather than assume an offset.
bool memoryContainsBytes(PC1500Machine& machine, const std::string& needle, uint16_t start, uint16_t end) {
    for (uint32_t addr = start; addr + needle.size() <= static_cast<uint32_t>(end) + 1; addr++) {
        bool match = true;
        for (size_t i = 0; i < needle.size(); i++) {
            if (machine.memory().peek(static_cast<uint16_t>(addr + i)) != static_cast<uint8_t>(needle[i])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

bool bootMachine(PC1500Machine& machine) {
    if (!machine.loadROMFile("roms/PC-1500_A04.ROM")) return false;
    machine.reset();
    // Matches PC1500PresetLoader.cpp's applyPC1500Preset() exactly: keys sent
    // immediately after reset() are missed entirely, since the ROM
    // doesn't start polling the keyboard until it settles into its
    // post-boot idle loop -- the unconditional settle run first is not
    // optional, per that file's own comment.
    machine.runCycles(static_cast<uint64_t>(1300000.0 * 2));
    waitIdle(machine, static_cast<uint64_t>(1300000.0 * 5));
    return true;
}

// typeBasicProgramText() adds to the resident program and clears nothing;
// a cold-booted machine needs CL + NEW0 first, as a preset's `keys:` would.
BasicTypeResult typeFreshProgram(PC1500Machine& machine, const std::string& text) {
    tapKey(machine, "cl");
    waitIdle(machine, static_cast<uint64_t>(PC1500Machine::kCpuHz * 2));
    std::string err;
    if (!typeLine(machine, "NEW0", /*pressEnter=*/true, &err)) {
        BasicTypeResult r;
        r.error = err;
        return r;
    }
    return typeBasicProgramText(machine, text);
}

void test_typeline_lowercase_via_shift() {
    PC1500Machine machine;
    if (!bootMachine(machine)) {
        std::fprintf(stderr, "SKIP test_typeline_lowercase_via_shift: roms/PC-1500_A04.ROM not found "
                              "relative to cwd (run tests from the repo root)\n");
        return;
    }

    BasicTypeResult result = typeFreshProgram(machine, "10 PRINT \"Bank: 7\"\n");
    CHECK(result.ok);
    CHECK(result.rejectedLines.empty());

    // The quoted string's literal bytes are stored verbatim (only BASIC
    // keywords like PRINT get tokenized) -- "Bank: 7" must survive with
    // its original case, not get silently uppercased to "BANK: 7"
    // (exercised by examples/ce163_bankswrm.pc1500a's own
    // '10 PRINT "Bank: 7"' line).
    CHECK(memoryContainsBytes(machine, "Bank: 7", 0x4000, 0x7FFF));
    CHECK(!memoryContainsBytes(machine, "BANK: 7", 0x4000, 0x7FFF));
}

void test_typeline_still_types_uppercase_directly() {
    PC1500Machine machine;
    if (!bootMachine(machine)) {
        std::fprintf(stderr, "SKIP test_typeline_still_types_uppercase_directly: roms/PC-1500_A04.ROM not found "
                              "relative to cwd (run tests from the repo root)\n");
        return;
    }

    // Typing already-uppercase text (the overwhelmingly common case --
    // BASIC keywords, most preset scripts) must work unchanged: the
    // SHIFT-tap only applies to 'a'-'z', never to 'A'-'Z'.
    BasicTypeResult result = typeFreshProgram(machine, "10 PRINT \"BANK: 7\"\n");
    CHECK(result.ok);
    CHECK(result.rejectedLines.empty());
    CHECK(memoryContainsBytes(machine, "BANK: 7", 0x4000, 0x7FFF));
}

// Several consecutive long lines must all be entered -- the BASIC line
// editor tokenises a line after ENTER without asserting BUSY, so typeLine()
// needs an unconditional post-ENTER settle or the next line's opening
// keystrokes land mid-tokenise and are dropped, and every line after the
// first gets rejected (the CE-163F UPDATERM.BAS DATA/POKE listing hit this).
void test_typebasicprogram_consecutive_long_lines() {
    PC1500Machine machine;
    if (!bootMachine(machine)) {
        std::fprintf(stderr, "SKIP test_typebasicprogram_consecutive_long_lines: roms/PC-1500_A04.ROM not found "
                              "relative to cwd (run tests from the repo root)\n");
        return;
    }

    // 5 x ~72-char DATA lines back to back, same shape as the real listing.
    BasicTypeResult result = typeFreshProgram(
        machine,
        "10 DATA 48008E08FFFFFFFFFFFF4801FD88BEEE716A696815B50BBEED004A7448155A00\n"
        "20 DATA 58416A98BE1565FD0A4204AE411C4C018B14B558AE4153AE415CAE4178AE417E\n"
        "30 DATA AE418BAE4193BE4100B5000A085657FDCA5CFF99075EFF990BFD88BE00E1BEEE\n"
        "40 REM CS: -30440 (0x8918) set X=9\n"
        "50 POKE &4100,&04,&8B,&52,&DF,&FD,&C8,&BE,&41,&57,&A5\n");

    CHECK(result.ok);
    CHECK(result.rejectedLines.empty());
    // The DATA payloads are stored verbatim -- spot-check the last line's.
    CHECK(memoryContainsBytes(machine, "AE418BAE4193BE4100", 0x4000, 0x7FFF));
    CHECK(memoryContainsBytes(machine, "&8B,&52,&DF", 0x4000, 0x7FFF));
}

// A long program whose tail is a long DATA line followed by a short line:
// after a long line's ENTER the line editor can park the PC high in ROM
// (display scroll / key settle) while it is *still* linking that line, so
// settleUntilIdle()'s structural idle test can return early -- with the
// program-end pointer not yet advanced. The per-line accept check must not
// mistake this for the ROM rejecting the line, and the next line's opening
// keystrokes must not land mid-tokenise and get dropped. This is the
// utilrm.bas tail (line 340 DATA + line 350 REM) from the CE-163F
// firmware-bootstrap preset.
void test_typebasicprogram_long_data_then_short_line_tail() {
    PC1500Machine machine;
    if (!bootMachine(machine)) {
        std::fprintf(stderr, "SKIP test_typebasicprogram_long_data_then_short_line_tail: roms/PC-1500_A04.ROM "
                              "not found relative to cwd (run tests from the repo root)\n");
        return;
    }

    // 33 x ~72-char DATA lines, then a long DATA line, then a short REM --
    // enough preceding program that the final DATA line's link lags the
    // false idle far enough to expose the early return.
    std::string prog;
    for (int n = 10; n <= 330; n += 10) {
        prog += std::to_string(n) +
                " DATA 6809AE15552EFD8A0E38BEE45189030799099AB5AAAE6809AE1555B555AE6808\n";
    }
    prog += "340 DATA AE2AAA9AFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF\n";
    prog += "350 REM CS: -9066 (0xdc96) set X=34\n";

    BasicTypeResult result = typeFreshProgram(machine, prog);

    CHECK(result.ok);
    CHECK(result.rejectedLines.empty());
    // The long DATA line linked...
    CHECK(memoryContainsBytes(machine, "AE2AAA9AFFFFFFFFFFFF", 0x0000, 0x7FFF));
    // ...and the short line after it was not dropped.
    CHECK(memoryContainsBytes(machine, "CS: -9066 (0xdc96)", 0x0000, 0x7FFF));
}

// typeLine() must not return until a typed line that *executes* BASIC has
// finished -- BUSY isn't asserted during a plain RUN, so without waiting
// for the interpreter to return to its idle loop the next scripted step
// would start against a still-running program and collide with it, as
// happens with the CE163F firmware-bootstrap preset's trailing `RUN` /
// `CALL` steps.
// ROM-gated: re-typing a line number with a same-length body replaces the
// line without moving BASPRG_END -- that is still a stored line.
void test_typebasicprogram_same_length_replacement_is_stored() {
    PC1500Machine machine;
    if (!bootMachine(machine)) {
        std::fprintf(stderr, "SKIP test_typebasicprogram_same_length_replacement_is_stored: roms/PC-1500_A04.ROM "
                              "not found relative to cwd (run tests from the repo root)\n");
        return;
    }
    BasicTypeResult result = typeFreshProgram(machine, "10 A=1\n10 A=2\n");
    CHECK(result.ok);
    CHECK(result.rejectedLines.empty());
}

// No ROM: LineStoreCheck over a hand-built program area.
void test_line_store_check() {
    // 10 A=1 / 20 B=2, records [no hi][no lo][len][content][0D].
    std::vector<uint8_t> mem = {0x00, 0x0A, 0x04, 'A', '=', '1', 0x0D,
                                0x00, 0x14, 0x04, 'B', '=', '2', 0x0D, 0xFF};
    auto peek = [&](uint32_t a) { return a < mem.size() ? mem[a] : uint8_t{0}; };
    const uint32_t end = 14;

    // Same-length rewrite of the typed line's record: changed.
    auto c20 = basic::LineStoreCheck::capture(peek, 0, end, "20 B=3");
    CHECK(!c20.changed(peek, end));
    mem[12] = '3';
    CHECK(c20.changed(peek, end));

    // A different line rewritten in place isn't the typed line's doing.
    auto c10 = basic::LineStoreCheck::capture(peek, 0, end, " 10 A=1");
    mem[12] = '4';
    CHECK(!c10.changed(peek, end));

    // A new line number (no resident record) counts only if the end moves;
    // so does a line with no number at all.
    auto c30 = basic::LineStoreCheck::capture(peek, 0, end, "30 C=3");
    CHECK(!c30.changed(peek, end));
    CHECK(c30.changed(peek, end + 7));
    auto direct = basic::LineStoreCheck::capture(peek, 0, end, "PRINT 1");
    CHECK(!direct.changed(peek, end));
}

void test_typeline_waits_for_run_to_finish() {
    PC1500Machine machine;
    if (!bootMachine(machine)) {
        std::fprintf(stderr, "SKIP test_typeline_waits_for_run_to_finish: roms/PC-1500_A04.ROM not found "
                              "relative to cwd (run tests from the repo root)\n");
        return;
    }

    // ~24 POKE lines into the &7C50+ ML scratch area (never touched by the
    // interpreter). Straight-line, but enough statement dispatch that RUN
    // takes well over typeLine()'s old fixed settle -- the shape of the
    // real CE163F UPDATERM.BAS bootstrap listing that exposed this.
    std::string prog;
    for (int i = 0; i < 24; i++) {
        prog += std::to_string(10 + i) + " POKE " + std::to_string(0x7C50 + i) + "," +
                std::to_string(0x41 + i) + "\n";
    }
    CHECK(typeFreshProgram(machine, prog).ok);

    tapKey(machine, "mode"); // PRO -> RUN
    // Run it, then immediately (no manual settle) issue a direct command.
    CHECK(typeLine(machine, "RUN", /*pressEnter=*/true, nullptr));
    CHECK(typeLine(machine, "POKE 31848,238", /*pressEnter=*/true, nullptr)); // &7C68 <- &EE

    // Every program line must have run before typeLine("RUN") returned...
    CHECK(machine.memory().peek(0x7C50) == 0x41); // first line
    CHECK(machine.memory().peek(0x7C67) == 0x41 + 23); // last line
    // ...and the trailing direct command must have landed cleanly on top.
    CHECK(machine.memory().peek(0x7C68) == 0xEE);
}

// waitUntilBasicIdle() reports "idle" only when the interpreter is really
// back spinning in its prompt command loop ($E2xx), and only after it has
// held there for the sustained ~0.33 s confirm window -- it never returns
// on the first frame that merely looks quiet (the property that keeps a
// CE-150 plot's pen-move busy-wait at $F7xx, which settleUntilIdle()'s
// structural test mistakes for idle, from ending a parameterless `- wait:`
// early). Checked here on a short program: the call still has to observe
// the full hold before returning, and lands with the PC in the prompt loop.
void test_wait_until_basic_idle_confirms_prompt_loop_hold() {
    PC1500Machine machine;
    if (!bootMachine(machine)) {
        std::fprintf(stderr, "SKIP test_wait_until_basic_idle_confirms_prompt_loop_hold: roms/PC-1500_A04.ROM "
                              "not found relative to cwd (run tests from the repo root)\n");
        return;
    }
    constexpr uint64_t kHz = 1300000;
    constexpr uint64_t kFrame = kHz / 60;

    CHECK(typeFreshProgram(machine,
        "10 POKE 31824,222\n"   // &7C50 <- &DE, so we can see the program ran
        "20 END\n").ok);
    tapKey(machine, "mode");                              // PRO -> RUN
    CHECK(typeLine(machine, "RUN", /*pressEnter=*/true, nullptr));
    CHECK(machine.memory().peek(0x7C50) == 0xDE);

    uint64_t spent = waitUntilBasicIdle(machine, kHz * 5);
    CHECK(spent < kHz * 5);                               // returned, didn't hit the cap
    CHECK(spent >= kFrame * 20);                          // but only after the sustained hold
    CHECK(machine.debugPC() >= 0xE200 && machine.debugPC() <= 0xE4FF); // in the prompt loop

    // A second call from an already-idle machine still waits out one full
    // confirm window and no more -- the hold is the whole mechanism.
    uint64_t again = waitUntilBasicIdle(machine, kHz * 5);
    CHECK(again >= kFrame * 20);
    CHECK(again < kFrame * 60);
}

} // namespace

int run_basictyper_tests() {
    test_typeline_lowercase_via_shift();
    test_typeline_still_types_uppercase_directly();
    test_typebasicprogram_consecutive_long_lines();
    test_typebasicprogram_long_data_then_short_line_tail();
    test_typebasicprogram_same_length_replacement_is_stored();
    test_line_store_check();
    test_typeline_waits_for_run_to_finish();
    test_wait_until_basic_idle_confirms_prompt_loop_hold();

    std::printf("basictyper_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
