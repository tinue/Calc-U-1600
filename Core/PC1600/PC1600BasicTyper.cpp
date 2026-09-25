#include "PC1600BasicTyper.hpp"

#include <cctype>
#include <sstream>

#include "../Basic/BasicLineStoreCheck.hpp"
#include "PC1600Display.hpp"
#include "PC1600Keyboard.hpp"
#include "PC1600Machine.hpp"
#include "PC1600StatusLine.hpp"
#include "PC1600TypedInput.hpp"

namespace {

// SC-7852 T-states at 3.58 MHz is this machine's canonical time unit
// (PC1600Machine::kTStateHz); one 60 Hz frame is the keystroke-pacing
// granularity, same as the PC-1500 typer's kCyclesPerFrame.
constexpr uint64_t kTStateHz = PC1600Machine::kTStateHz;
constexpr int kFramesPerSecond = 60;
constexpr uint64_t kFrameTStates = kTStateHz / kFramesPerSecond;

// One keystroke: hold, then idle, a few frames each -- the cadence the
// PC-1600 preset loader already used, and a match for the PC-1500 typer's
// 4-frame hold + 4-frame gap.
constexpr int kTapFrames = 4;
constexpr int kIdleFrames = 4;

// SHIFT is a one-shot latch: tap it, give the ROM's key-scan a real gap
// to notice and latch it, then tap the base key (which consumes the
// latch). ~100 ms, the gap interactive shifted keys have always used.
constexpr int kShiftGapFrames = 6;

// Settle after a line's ENTER, from typeLine() -- a short interval (poll
// BUSY briefly in case a `type:` line ran real BASIC, then a fixed floor).
// typeBasicProgramText() adds settleUntilProgramPtrStable() on top of this
// for program lines. A preset that types a line which executes a long-
// running program should still add an explicit `- wait:` after it.
constexpr uint64_t kPostEnterBusyCap = kTStateHz;        // 1 s
constexpr int kPostEnterFloorFrames = 12;                // ~0.2 s

// BASIC program-end pointer -- 2 bytes, BIG-ENDIAN, at the PC-1600's
// SC-7852-view work-area address F867H (the PC-1500's $7867 BASPRG_END,
// carried over per PC-1600-Work-Area-Map.md's "Block C is used exactly as
// on the PC-1500/1500A"). CONFIRMED against the real ROM set: in PRO mode
// it advances by each stored line's tokenised size; in RUN mode it stays
// put (a typed line is a direct command, not stored). C000H-FFFFH is the
// fixed internal RAM (no banking), so machine.memory().peek() resolves it.
constexpr uint16_t kProgramEndPtr = 0xF867;

uint16_t readBE16(PC1600Machine& machine, uint16_t addr) {
    uint16_t hi = machine.memory().peek(addr);
    uint16_t lo = machine.memory().peek(static_cast<uint16_t>(addr + 1));
    return static_cast<uint16_t>((hi << 8) | lo);
}

// F865/F867 (BASPRG_ST/BASPRG_END) hold LH5803-side addresses; the
// LH5803's $0000-$7FFF aliases the SC-7852's $8000-$FFFF (same mapping as
// pc1600_cli --dump-basic).
constexpr uint16_t kProgramStartPtr = 0xF865;

uint32_t toZ80(uint16_t a) { return a < 0x8000 ? a + 0x8000u : a; }

uint8_t peekZ80(PC1600Machine& machine, uint32_t a) {
    return machine.memory().peek(static_cast<uint16_t>(a));
}

basic::LineStoreCheck captureProgram(PC1600Machine& machine, const std::string& line) {
    return basic::LineStoreCheck::capture([&](uint32_t a) { return peekZ80(machine, a); },
                                          toZ80(readBE16(machine, kProgramStartPtr)),
                                          toZ80(readBE16(machine, kProgramEndPtr)), line);
}

bool programChanged(PC1600Machine& machine, const basic::LineStoreCheck& check) {
    return check.changed([&](uint32_t a) { return peekZ80(machine, a); },
                         toZ80(readBE16(machine, kProgramEndPtr)));
}

// Run frames until the program-end pointer has held the same value for a
// sustained stretch (or a cap). The line editor links a line after ENTER
// without asserting BUSY, so waitIdle() alone can return before F867 has
// settled and the next line's keystrokes would collide with the still-
// running tokeniser. Mirrors PC1500BasicTyper's settleUntilProgramPtrStable.
void settleUntilProgramPtrStable(PC1600Machine& machine) {
    constexpr int kStableFramesNeeded = 30;                         // ~0.5 s of no change
    constexpr uint64_t kCap = static_cast<uint64_t>(kTStateHz * 6); // ~6 s emulated

    uint64_t spent = 0;
    int stable = 0;
    uint16_t last = readBE16(machine, kProgramEndPtr);
    while (stable < kStableFramesNeeded && spent < kCap) {
        spent += machine.runCycles(kFrameTStates);
        uint16_t now = readBE16(machine, kProgramEndPtr);
        if (now == last) stable++;
        else { stable = 0; last = now; }
    }
}

} // namespace

void tapKey(PC1600Machine& machine, const std::string& name) {
    machine.pressKey(name);
    machine.runCycles(kFrameTStates * kTapFrames);
    machine.releaseKey(name);
    machine.runCycles(kFrameTStates * kIdleFrames);
}

uint64_t waitIdle(PC1600Machine& machine, uint64_t maxTStates) {
    uint64_t consumed = 0;
    while (machine.display().statusLine().isOn(PC1600StatusLine::Symbol::Busy) && consumed < maxTStates) {
        consumed += machine.runCycles(kFrameTStates);
    }
    return consumed;
}

void waitForKeyboardScanLoop(PC1600Machine& machine) {
    // Needed with ANY plotter, not just the CE-1600P. The CE-1600P's
    // power-on init opens the gap on the SC7852 side; the CE-150 opens the
    // same gap a different way -- the PC-1600 boot's plotter self-test
    // hands off to the LH5803 to home the CE-150 turret, and the SC7852
    // key-scan loop only re-engages for good once that returns. Either way
    // the first scripted keystroke would otherwise land in a dead scan.
    if (!machine.ce1600pAttached() && !machine.ce150Attached()) return;

    //
    // "Ready" per frame: CK0 (the LCD clock) is on, and the SC7852 is either
    // in the BASIC command loop (pc1600AtBasicPrompt) or has read the key
    // matrix (port 37H) at least once this frame. The idle loop polls 37H
    // only ~0-1 times per frame, so the command-loop PC is the primary
    // signal; the scan read covers key: steps taken outside it (RSV mode,
    // INPUT, ...). An earlier ">= 3 scans per frame" test never held and
    // ran every call into the cap.
    constexpr int kActiveFramesNeeded = 15;                           // held ~0.25 s
    constexpr uint64_t kCap = static_cast<uint64_t>(kTStateHz * 15);  // safety only

    uint64_t spent = 0;
    int active = 0;
    uint64_t lastScan = machine.keyboard().scanCount();
    while (active < kActiveFramesNeeded && spent < kCap) {
        spent += machine.runCycles(kFrameTStates);
        uint64_t nowScan = machine.keyboard().scanCount();
        const bool polling = nowScan != lastScan;
        lastScan = nowScan;
        const bool ready = pc1600AtBasicPrompt(machine) || polling;
        if (ready && machine.display().clockEnabled()) active++;
        else active = 0;
    }
}

void runBootToPrompt(PC1600Machine& machine) {
    // Generous margin past the boot ROM's power-on sequence -- the ROM
    // doesn't poll the keyboard until it settles into its post-boot idle
    // loop (headless: ~2M T-states to a stable PC set, see
    // tools/pc1600_cli.cpp) -- then a BUSY-symbol idle poll for the tail of
    // a slower boot.
    constexpr uint64_t kBootSettleTStates = PC1600Machine::kTStateHz * 2;
    constexpr uint64_t kBootIdleCapTStates = PC1600Machine::kTStateHz * 5;
    machine.runCycles(kBootSettleTStates);
    waitIdle(machine, kBootIdleCapTStates);
    // With a plotter attached the boot ROM only turns the LCD on and enters
    // its keyboard-scan idle loop once the CE-1600P has finished its power-
    // on init (on real hardware the LCD + indicator strip stay dark until
    // then) -- that init raises no BUSY symbol. No-op without a plotter.
    waitForKeyboardScanLoop(machine);
    // The keyboard-scan test above can pass while the ROM is still finishing
    // its start-up (measured with a CE-1600P: ~10 s of emulated time before
    // the cursor appears), so also wait until it is steady in the BASIC
    // command loop -- otherwise that tail runs in real time.
    waitUntilBasicIdle(machine, PC1600Machine::kTStateHz * 20);  // cap: safety only
}

uint64_t waitUntilBasicIdle(PC1600Machine& machine, uint64_t maxTStates) {
    // "The program has finished" == the SC7852 is back spinning in the BASIC
    // command loop, a very tight loop pinned to ~$92B3 (measured: at the
    // prompt the SC7852 PC is on that exact address ~97% of frames; a
    // running program, an INPUT wait, a plot pen-move wait, and a FOR/NEXT
    // delay all sit in *other* tight loops -- $53xx, $AAxx, ... -- so
    // "small stable PC span" alone is not enough to tell them apart).
    // The PC-1600 ROM set is fixed, so this address is stable; the window
    // is generous and the hold long, so a transient BREAK-poll visit during
    // a run doesn't count.
    // The address window itself lives in pc1600AtBasicPrompt().
    constexpr int kFramesNeeded = 20;  // ~0.33 s continuously in the command loop

    uint64_t spent = 0;
    int inLoop = 0;
    while (inLoop < kFramesNeeded && spent < maxTStates) {
        spent += machine.runCycles(kFrameTStates);
        if (pc1600AtBasicPrompt(machine)) inLoop++;
        else inLoop = 0;
    }
    return spent;
}

bool typeLine(PC1600Machine& machine, const std::string& line, bool pressEnter, std::string* error) {
    // Don't start typing until the ROM's key-scan loop is actually running
    // -- after the previous line's ENTER (tokenisation, plus CE-1600P post-
    // line housekeeping) it drops out of the loop, and the first characters
    // of this line would otherwise land unseen. No-op without a plotter.
    waitForKeyboardScanLoop(machine);

    for (char c : line) {
        std::string name;
        bool needsShift = false;
        if (!pc1600ResolveTypedChar(c, &name, &needsShift)) {
            if (error) *error = "no PC-1600 key for character '" + std::string(1, c) + "'";
            return false;
        }
        if (needsShift) {
            // SHIFT tapped (not held) immediately before the base key; the
            // latch is consumed by that key and must NOT be explicitly
            // un-latched afterward (an explicit un-latch corrupts the next
            // unshifted character -- confirmed on the PC-1500 side).
            tapKey(machine, "shift");
            machine.runCycles(kFrameTStates * kShiftGapFrames);
        }
        tapKey(machine, name);
    }
    if (pressEnter) {
        tapKey(machine, "enter");
        waitIdle(machine, kPostEnterBusyCap);
        machine.runCycles(kFrameTStates * kPostEnterFloorFrames);
    } else {
        waitIdle(machine, kPostEnterBusyCap);
    }
    return true;
}

BasicTypeResult typeBasicProgramText(PC1600Machine& machine, const std::string& text) {
    BasicTypeResult result;

    int typedLines = 0;   // lines actually sent to the editor (not the length-guard skips)
    int storedCount = 0;  // of those, how many changed the program

    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue; // blank source lines are just skipped

        // An over-length line would be truncated by the editor and stored
        // wrong -- reject it up front rather than type a mangled line.
        if (static_cast<int>(line.size()) > kMaxBasicLineLength) {
            result.rejectedLines.push_back(line);
            continue;
        }

        const basic::LineStoreCheck before = captureProgram(machine, line);

        std::string typeError;
        if (!typeLine(machine, line, /*pressEnter=*/true, &typeError)) {
            result.error = typeError;
            return result;
        }
        typedLines++;

        // Wait for the editor to finish linking the line, then check it
        // actually landed: a stored line moves F867 (BASPRG_END,
        // big-endian) or, replacing a line of the same tokenised length,
        // rewrites that line's record. A line that changed neither was not
        // stored -- almost always because the machine isn't in PRO mode (a
        // preset must `key: mode` into it before the program: block).
        // Re-typing a line identical to the stored one also changes
        // nothing and is reported the same way.
        settleUntilProgramPtrStable(machine);
        if (programChanged(machine, before)) storedCount++;
        else result.rejectedLines.push_back(line);
    }

    result.ok = result.rejectedLines.empty();
    if (!result.ok && result.error.empty()) {
        if (typedLines > 0 && storedCount == 0) {
            result.error = "no program line was stored -- is the machine in PRO mode? "
                           "(a preset must `key: mode` into PRO before a program: block)";
        } else {
            result.error = std::to_string(result.rejectedLines.size()) +
                           " program line(s) not stored (too long, or rejected by the editor)";
        }
    }
    return result;
}
