#include "PC1500BasicTyper.hpp"

#include <cctype>
#include <sstream>

#include "../Basic/BasicLineStoreCheck.hpp"
#include "PC1500Keyboard.hpp"
#include "PC1500Machine.hpp"
#include "PC1500TypedInput.hpp"

namespace {

constexpr double kCpuHz = PC1500Machine::kCpuHz;
constexpr uint64_t kCyclesPerFrame = kPC1500CyclesPerFrame;

// Key-scan cadence: kTapFrames=4 hold + kIdleFrames=4 idle, ~67ms per
// keystroke at 60fps -- see this file's header comment.
constexpr int kTapFrames = 4;
constexpr int kIdleFrames = 4;

// Program start/end pointers (BASPRG_ST/BASPRG_END): 2 bytes each,
// big-endian; the end pointer is updated whenever the BASIC program area
// changes size.
constexpr uint16_t kProgramStartPtr = 0x7865;
constexpr uint16_t kProgramEndPtr = 0x7867;

uint16_t readBE16(PC1500Machine& machine, uint16_t addr) {
    uint16_t hi = machine.memory().peek(addr);
    uint16_t lo = machine.memory().peek(static_cast<uint16_t>(addr + 1));
    return static_cast<uint16_t>((hi << 8) | lo);
}


// Run frames until the BASIC program-end pointer has held the same value for
// a sustained stretch (or a cap is hit). settleUntilIdle()'s structural
// PC-range test can be fooled mid-edit: the line editor briefly parks the PC
// high in ROM (display scroll / key settle) while it is still linking the
// line just entered, so settleUntilIdle() can return with kProgramEndPtr not
// yet updated. Sampling the pointer directly is unambiguous -- once it stops
// moving, the line really is linked and the next line's keystrokes are safe
// to send. Cheap for short lines (pointer settles almost immediately);
// matters for long DATA lines, where the link lags the false idle by ~0.5s.
job::Task<void> settleUntilProgramPtrStable(PC1500Machine& machine) {
    constexpr int kStableFramesNeeded = 30;        // ~0.5s of no change
    constexpr uint64_t kCap = static_cast<uint64_t>(kCpuHz * 6); // ~6s emulated

    uint64_t spent = 0;
    int stable = 0;
    uint16_t last = readBE16(machine, kProgramEndPtr);
    while (stable < kStableFramesNeeded && spent < kCap) {
        spent += co_await job::run(machine, kCyclesPerFrame);
        uint16_t now = readBE16(machine, kProgramEndPtr);
        if (now == last) stable++;
        else { stable = 0; last = now; }
    }
}

} // namespace

// Run cycles until the BASIC interpreter is back idling in its ROM command
// loop, or `maxCycles` is hit. Needed because the BUSY annunciator (what
// waitIdle() watches) is NOT asserted for line tokenisation OR for a plain
// program RUN, so after `type: RUN` (or any typed line that executes real
// BASIC) the next scripted step would otherwise start against a
// still-running program and collide with it (observed: the CE163F
// firmware-bootstrap preset's trailing RUN/CALL steps failing, the flash
// left unwritten). A parameterless `- wait:` preset step also uses this,
// with a much larger cap, to block until a long program / plot finishes.
//
// "Idle" is detected structurally, with no ROM-version-specific address
// baked in: sample the PC across a few frames and call it idle once the
// visited range collapses to a small window sitting high in ROM (the
// command loop lives around $E2xx on A04; a running interpreter dispatching
// statements sweeps most of $C000-$FAxx). Not covered: a tight
// single-line delay loop (`FOR I=1 TO big : NEXT`), whose NEXT fast-path
// never leaves the same $E2xx dispatch code the idle prompt uses, and a
// long-spinning `CALL` into machine code -- both want an explicit `wait: N`
// step in the preset. Returns the cycles consumed.
job::Task<uint64_t> settleUntilIdleJob(PC1500Machine& machine, uint64_t maxCycles) {
    constexpr int kWindowFrames = 4;               // frames per PC-range sample
    constexpr int kQuietWindowsNeeded = 3;         // consecutive quiet samples (~0.2s)
    constexpr uint16_t kMaxSpan = 0x300;           // "small window"
    constexpr uint16_t kIdleFloor = 0xE000;        // command loop lives above this

    uint64_t spent = 0;
    int quiet = 0;
    while (quiet < kQuietWindowsNeeded && spent < maxCycles) {
        uint16_t lo = 0xFFFF, hi = 0;
        const uint64_t edgesBefore = machine.buzzerEdgeCount();
        for (int i = 0; i < kWindowFrames; i++) {
            spent += co_await job::run(machine, kCyclesPerFrame);
            uint16_t pc = machine.debugPC();
            if (pc < lo) lo = pc;
            if (pc > hi) hi = pc;
        }
        // A BEEP's tone loop (A04 E66Aff) is itself a small window high in
        // ROM, so the PC test alone takes a sounding buzzer for the idle
        // prompt and the next typed line lands mid-beep. A window in which
        // the buzzer line toggled is never idle.
        const bool beeping = machine.buzzerEdgeCount() != edgesBefore;
        if (hi - lo < kMaxSpan && lo >= kIdleFloor && !beeping) quiet++;
        else quiet = 0;
    }
    co_return spent;
}

job::Task<uint64_t> waitUntilBasicIdleJob(PC1500Machine& machine, uint64_t maxCycles) {
    // "The program / plot has finished" == the interpreter is back in its
    // prompt command loop, which idles as a HLT-per-frame spin pinned to
    // ~$E2AA. Measured on A04: at the `>` prompt the end-of-frame PC is
    // $E2AA for ~90% of frames and otherwise still within $E2xx-$E4xx (the
    // loop body + the RTC / key-scan interrupt service it wakes into);
    // while a CE-150 plot runs it is never in that range -- it sits in a
    // tight pen-move busy-wait at $F7xx, sweeps math code at $A_xx, etc.
    // "small stable PC window high in ROM" (settleUntilIdle's test) can't
    // tell that busy-wait from the prompt, so this checks the prompt-loop
    // address directly. The ROM set is fixed, so the window is safe to
    // bake in; the hold is long enough that a between-statement dispatch
    // visit during a run doesn't count.
    // The address window itself lives in pc1500AtBasicPrompt().
    constexpr int kFramesNeeded = 20;   // ~0.33 s continuously at the prompt

    uint64_t spent = 0;
    int inLoop = 0;
    while (inLoop < kFramesNeeded && spent < maxCycles) {
        spent += co_await job::run(machine, kCyclesPerFrame);
        if (pc1500AtBasicPrompt(machine)) inLoop++;
        else inLoop = 0;
    }
    co_return spent;
}

job::Task<void> tapKeyJob(PC1500Machine& machine, std::string name) {
    machine.pressKey(name);
    co_await job::run(machine, kCyclesPerFrame * kTapFrames);
    machine.releaseKey(name);
    co_await job::run(machine, kCyclesPerFrame * kIdleFrames);
}

job::Task<uint64_t> waitIdleJob(PC1500Machine& machine, uint64_t maxCycles) {
    uint64_t consumed = 0;
    while (machine.display().statusLine().isOn(StatusLine::Symbol::Busy) && consumed < maxCycles) {
        consumed += co_await job::run(machine, kCyclesPerFrame);
    }
    co_return consumed;
}

job::Task<void> runBootToPromptJob(PC1500Machine& machine) {
    // Generous margin past the ROM's own power-on RAM-check/boot sequence --
    // keys sent immediately after reset are missed entirely, since the ROM
    // doesn't start polling the keyboard until it settles into its post-boot
    // idle loop.
    constexpr uint64_t kBootSettleCycles = static_cast<uint64_t>(kCpuHz * 2);
    constexpr uint64_t kBootIdleCap = static_cast<uint64_t>(kCpuHz * 5);
    co_await job::run(machine, kBootSettleCycles);
    co_await waitIdleJob(machine, kBootIdleCap);
}

job::Task<bool> typeLineJob(PC1500Machine& machine, std::string line, bool pressEnter, std::string* error) {
    for (char c : line) {
        std::string name;
        bool needsShift = false;
        if (!pc1500ResolveTypedChar(c, &name, &needsShift)) {
            if (error) *error = "no keyboard mapping for character '" + std::string(1, c) + "'";
            co_return false;
        }
        if (needsShift) {
            // SHIFT is a one-shot latch, tapped immediately before the
            // base key -- not held, and (confirmed empirically: an
            // explicit un-latch tap before an unshifted character that
            // immediately follows a shifted one causes a real syntax
            // error, e.g. typing `A$=` produced "@" instead of "=") the
            // latch is consumed by whatever key is pressed next and
            // doesn't need to be explicitly cleared afterward.
            co_await tapKeyJob(machine, "shift");
        }
        co_await tapKeyJob(machine, name);
    }
    if (pressEnter) {
        co_await tapKeyJob(machine, "enter");
        // The line editor tokenises/links WITHOUT asserting BUSY, and a
        // `RUN`/`CALL` that executes real BASIC doesn't assert it either, so
        // waitIdle() alone returns immediately and the next scripted step
        // would start mid-work and collide with it (line entry: dropped
        // keystrokes -> rejected lines; RUN: the trailing steps run against
        // a still-executing program). Wait for the interpreter to be back
        // idling in its ROM command loop instead.
        co_await settleUntilIdleJob(machine, static_cast<uint64_t>(kCpuHz * 12));  // ~12 s emulated
    } else {
        co_await waitIdleJob(machine, static_cast<uint64_t>(kCpuHz * 2));
    }
    co_return true;
}

job::Task<BasicTypeResult> typeBasicProgramTextJob(PC1500Machine& machine, std::string text) {
    BasicTypeResult result;

    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue; // blank source lines are just skipped, matching a human not typing an empty statement

        auto peek = [&](uint32_t a) { return machine.memory().peek(static_cast<uint16_t>(a)); };
        const basic::LineStoreCheck before = basic::LineStoreCheck::capture(
            peek, readBE16(machine, kProgramStartPtr), readBE16(machine, kProgramEndPtr), line);

        std::string typeError;
        if (!co_await typeLineJob(machine, line, /*pressEnter=*/true, &typeError)) {
            result.error = typeError;
            co_return result;
        }

        // typeLine()'s own settleUntilIdle() can return while the line editor
        // is still linking a long line (see settleUntilProgramPtrStable's
        // comment) -- reading the pointer here would then both misreport the
        // line as rejected AND let the next line's keystrokes collide with
        // the still-running tokeniser. Wait for the pointer to actually
        // settle before judging acceptance or moving on. A same-length
        // replacement leaves the pointer put and only rewrites its line,
        // which LineStoreCheck also compares.
        co_await settleUntilProgramPtrStable(machine);
        if (!before.changed(peek, readBE16(machine, kProgramEndPtr))) {
            result.rejectedLines.push_back(line);
        }
    }

    result.ok = result.rejectedLines.empty();
    if (!result.ok && result.error.empty()) {
        result.error = std::to_string(result.rejectedLines.size()) + " line(s) rejected by the ROM";
    }
    co_return result;
}

// ---- Blocking forms ----

uint64_t settleUntilIdle(PC1500Machine& machine, uint64_t maxCycles) {
    return job::runToCompletion(settleUntilIdleJob(machine, maxCycles));
}

uint64_t waitUntilBasicIdle(PC1500Machine& machine, uint64_t maxCycles) {
    return job::runToCompletion(waitUntilBasicIdleJob(machine, maxCycles));
}

void tapKey(PC1500Machine& machine, const std::string& name) { job::runToCompletion(tapKeyJob(machine, name)); }

uint64_t waitIdle(PC1500Machine& machine, uint64_t maxCycles) {
    return job::runToCompletion(waitIdleJob(machine, maxCycles));
}

void runBootToPrompt(PC1500Machine& machine) { job::runToCompletion(runBootToPromptJob(machine)); }

bool typeLine(PC1500Machine& machine, const std::string& line, bool pressEnter, std::string* error) {
    return job::runToCompletion(typeLineJob(machine, line, pressEnter, error));
}

BasicTypeResult typeBasicProgramText(PC1500Machine& machine, const std::string& text) {
    return job::runToCompletion(typeBasicProgramTextJob(machine, text));
}
