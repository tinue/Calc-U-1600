// Headless tests for the debugger's run control (Core/Debug/RunControl) and
// breakpoint table (Core/Debug/BreakpointTable), driven with the real
// memtest program (examples/memtest_stock.bin, ENTRY 0x40C5) and its sdas
// listing fixture on a PC-1500A with no ROM. A small harness calls it:
//
//   4600  sjp 0x40C5      ; BE 40 C5
//   4603  bch 0x4603      ; 9E 02  -- parks here when memtest returns
//
// memtest tests the RAM between BOTTOM (&7867/8) and RAM_END_H (&7864),
// set up here as 4200-43FF, once (X = 1).
//
// Build & run: see tools/run_tests.sh (from the repo root)

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../Debug/BreakpointTable.hpp"
#include "../Debug/Listing/Listing.hpp"
#include "../Debug/MachineDebugTargets.hpp"
#include "../Debug/RunControl.hpp"
#include "../Debug/SourceMap.hpp"
#include "../PC1500/PC1500Machine.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// One PC-1500A with memtest + harness loaded, its listing bound as a load,
// and the debugger objects around it.
struct Rig {
    PC1500Machine machine;
    debug::PC1500DebugTarget target{machine};
    debug::SourceMap map;
    debug::BreakpointTable breakpoints;
    debug::RunControl rc{target, map, breakpoints};
    std::string asmFile;
    bool ok = false;

    Rig() {
        std::ifstream bin("examples/memtest_stock.bin", std::ios::binary);
        std::vector<uint8_t> image((std::istreambuf_iterator<char>(bin)), std::istreambuf_iterator<char>());
        debug::Listing listing;
        std::string error;
        if (image.empty() || !debug::loadListing("Core/tests/fixtures/listings/sdas-lh5801/memtest.rst", &listing, &error))
            return;
        asmFile = listing.files[0];
        ok = true;
        for (size_t i = 0; i < image.size(); i++) ok = ok && machine.memory().poke(uint16_t(0x40C5 + i), image[i]);
        const uint8_t harness[] = {0xBE, 0x40, 0xC5, 0x9E, 0x02};
        for (size_t i = 0; i < sizeof harness; i++) ok = ok && machine.memory().poke(uint16_t(0x4600 + i), harness[i]);
        ok = ok && machine.memory().poke(0x7867, 0x42) && machine.memory().poke(0x7868, 0x00) &&
             machine.memory().poke(0x7864, 0x44);
        LH5801& cpu = machine.cpu();
        cpu.setPC(0x4600);
        cpu.setSP(0x7AFF);
        cpu.setX(0x0001);
        const int id = map.addLoaded(1, listing, {}, 0x40C5, uint16_t(0x40C5 + image.size() - 1), "memtest.rst");
        ok = ok && map.verify(id, rc.bankMatch(), rc.codePeek()) == 0;
    }

    std::vector<debug::BreakpointStatus> lineBreakpoint(int line, const std::string& condition = {},
                                                        const std::string& hit = {}, const std::string& log = {}) {
        debug::BreakpointTable::SourceRequest r;
        r.line = line;
        r.condition = condition;
        r.hitCondition = hit;
        r.logMessage = log;
        auto st = breakpoints.setSource(asmFile, {r}, map);
        breakpoints.apply(target);
        return st;
    }

    void clearBreakpoints() {
        breakpoints.clear();
        breakpoints.apply(target);
    }

    // Runs slices until the run control stops; returns every event.
    std::vector<debug::DebugEvent> runUntilStopped(int maxSlices = 2000) {
        std::vector<debug::DebugEvent> all;
        for (int i = 0; i < maxSlices && !rc.paused(); i++) {
            auto ev = rc.slice(20000);
            all.insert(all.end(), ev.begin(), ev.end());
        }
        if (std::getenv("RC_TRACE")) {
            for (const auto& e : all)
                if (e.kind == debug::DebugEvent::Stopped) std::fprintf(stderr, "  stop reason=%d thread=%d\n", e.reason, e.thread);
            std::fprintf(stderr, "  -> paused=%d pc=%04X events=%zu\n", rc.paused(), target.pc(1), all.size());
        }
        return all;
    }

    int line() {
        debug::SourceLocation loc;
        return rc.locate(1, target.pc(1), &loc) ? loc.line : 0;
    }
};

const debug::DebugEvent* lastStop(const std::vector<debug::DebugEvent>& events) {
    for (size_t i = events.size(); i-- > 0;)
        if (events[i].kind == debug::DebugEvent::Stopped) return &events[i];
    return nullptr;
}

size_t outputs(const std::vector<debug::DebugEvent>& events) {
    size_t n = 0;
    for (const auto& e : events) n += e.kind == debug::DebugEvent::Output;
    return n;
}

void test_line_breakpoint_and_line_steps() {
    Rig r;
    if (!r.ok) { std::printf("  SKIP run control tests: memtest fixture missing\n"); return; }
    // Line 88 is a comment: the breakpoint resolves to 89 (ldi a,0x00).
    auto st = r.lineBreakpoint(88);
    CHECK(st.size() == 1 && st[0].verified && st[0].line == 89 && st[0].addr == 0x40CC);
    r.rc.resume();
    auto ev = r.runUntilStopped();
    const debug::DebugEvent* stop = lastStop(ev);
    CHECK(stop && stop->reason == debug::DebugEvent::Breakpoint && stop->breakpointIds == std::vector<int>{st[0].id});
    CHECK(r.target.pc(1) == 0x40CC && r.line() == 89);

    // Line steps: 89 -> 90 -> 94 (91-93 are blank/comments).
    r.rc.step(1, debug::RunControl::StepKind::Over, /*line=*/true);
    ev = r.runUntilStopped();
    stop = lastStop(ev);
    CHECK(stop && stop->reason == debug::DebugEvent::Step && r.line() == 90);
    r.rc.step(1, debug::RunControl::StepKind::In, true);
    r.runUntilStopped();
    CHECK(r.line() == 94 && r.target.pc(1) == 0x40D1);

    // Instruction step.
    r.rc.step(1, debug::RunControl::StepKind::Instruction, false);
    r.runUntilStopped();
    CHECK(r.target.pc(1) == 0x40D2);

    // Step out of memtest: back in the harness after its rtn.
    r.clearBreakpoints();
    r.rc.step(1, debug::RunControl::StepKind::Out, false);
    ev = r.runUntilStopped(20000);
    stop = lastStop(ev);
    CHECK(stop && stop->reason == debug::DebugEvent::Step);
    CHECK(r.target.pc(1) == 0x4603 && r.line() == 0);
    uint8_t flag = 0xFF;
    CHECK(r.target.peek(1, debug::kSpaceMain, 0x40C7, &flag) && flag == 0); // ERR_FLAG: pass
}

void test_step_over_a_call() {
    Rig r;
    if (!r.ok) return;
    // At the harness: step over `sjp 0x40C5` runs all of memtest.
    r.rc.step(1, debug::RunControl::StepKind::Over, false);
    auto ev = r.runUntilStopped(20000);
    const debug::DebugEvent* stop = lastStop(ev);
    CHECK(stop && stop->reason == debug::DebugEvent::Step && r.target.pc(1) == 0x4603);

    // A breakpoint inside the call still stops the step over.
    r.target.writeRegister(1, "p", 0x4600);
    r.target.writeRegister(1, "x", 1);
    r.lineBreakpoint(90);
    r.rc.step(1, debug::RunControl::StepKind::Over, false);
    ev = r.runUntilStopped(20000);
    stop = lastStop(ev);
    CHECK(stop && stop->reason == debug::DebugEvent::Breakpoint && r.target.pc(1) == 0x40CE);
}

void test_conditions_hit_counts_and_logpoints() {
    Rig r;
    if (!r.ok) return;
    // Line 137 is `sta (x)` in pass 1's loop, once per tested byte.
    r.lineBreakpoint(137, "x == 0x4210");
    r.rc.resume();
    auto ev = r.runUntilStopped();
    const debug::DebugEvent* stop = lastStop(ev);
    uint32_t x = 0;
    CHECK(stop && stop->reason == debug::DebugEvent::Breakpoint);
    CHECK(r.target.readRegister(1, "x", &x) && x == 0x4210);

    // Hit condition: the third hit after re-arming.
    r.lineBreakpoint(137, {}, "== 3");
    r.rc.resume();
    r.runUntilStopped();
    CHECK(r.target.readRegister(1, "x", &x) && x == 0x4213);

    // A logpoint prints and never stops: 0x4214..0x43FF remain in pass 1.
    r.lineBreakpoint(137, {}, {}, "x={x} a={a}");
    debug::BreakpointTable::InstructionRequest park;
    park.addr = 0x4603;
    r.breakpoints.setInstructions({park});
    r.breakpoints.apply(r.target);
    r.rc.resume();
    ev = r.runUntilStopped(20000);
    stop = lastStop(ev);
    CHECK(stop && r.target.pc(1) == 0x4603);
    CHECK(outputs(ev) == 0x4400 - 0x4214);
    bool firstOk = false;
    for (const auto& e : ev)
        if (e.kind == debug::DebugEvent::Output) { firstOk = e.text == "x=0x4214 a=0x55"; break; }
    CHECK(firstOk);

    // A broken condition stops and says why.
    r.target.writeRegister(1, "p", 0x4600);
    r.lineBreakpoint(89, "nosuch == 1");
    r.rc.resume();
    ev = r.runUntilStopped();
    CHECK(lastStop(ev) && r.target.pc(1) == 0x40CC && outputs(ev) == 1);
}

void test_data_breakpoint_and_pause() {
    Rig r;
    if (!r.ok) return;
    debug::DataBreakpointSpec w;
    w.addr = 0x40C7; // ERR_FLAG: first written by `sta (ERR_FLAG)` at 40CE
    w.access = debug::DataAccess::Write;
    auto st = r.breakpoints.setData({w});
    r.breakpoints.apply(r.target);
    r.rc.resume();
    auto ev = r.runUntilStopped();
    const debug::DebugEvent* stop = lastStop(ev);
    CHECK(stop && stop->reason == debug::DebugEvent::DataBreakpoint && stop->breakpointIds == std::vector<int>{st[0].id});
    CHECK(r.target.pc(1) == 0x40D1); // after the storing instruction

    r.breakpoints.setData({});
    r.breakpoints.apply(r.target);
    r.rc.resume();
    CHECK(r.rc.slice(1000).empty() && !r.rc.paused());
    r.rc.pause();
    ev = r.rc.slice(1000);
    CHECK(ev.size() == 1 && ev[0].reason == debug::DebugEvent::Pause && r.rc.paused());
}

void test_bank_qualified_listing() {
    Rig r;
    if (!r.ok) return;
    // The same listing bound statically for PV=1 only: with PV=0 its
    // breakpoints are armed but never stop.
    debug::Listing listing;
    std::string error;
    CHECK(debug::loadListing("Core/tests/fixtures/listings/sdas-lh5801/memtest.rst", &listing, &error));
    r.map.clear();
    r.map.addStatic(1, listing, {-1, -1, -1, 1}, "pv1");
    auto st = r.lineBreakpoint(89);
    CHECK(st[0].verified && st[0].message.find("pv=1") != std::string::npos);
    debug::BreakpointTable::InstructionRequest park;
    park.addr = 0x4603;
    r.breakpoints.setInstructions({park});
    r.breakpoints.apply(r.target);
    r.rc.resume();
    r.runUntilStopped(20000);
    CHECK(r.target.pc(1) == 0x4603);
    // With PV=1 the same breakpoint stops.
    r.target.writeRegister(1, "p", 0x4600);
    r.target.writeRegister(1, "pv", 1);
    r.rc.resume();
    auto ev = r.runUntilStopped();
    const debug::DebugEvent* stop = lastStop(ev);
    CHECK(stop && stop->reason == debug::DebugEvent::Breakpoint && r.target.pc(1) == 0x40CC);
}

void test_hit_condition_parsing() {
    CHECK(debug::hitConditionMet("3", 3) && !debug::hitConditionMet("3", 4));
    CHECK(debug::hitConditionMet(">= 5", 5) && !debug::hitConditionMet(">=5", 4));
    CHECK(debug::hitConditionMet("% 3", 6) && !debug::hitConditionMet("%3", 7));
    CHECK(debug::hitConditionMet("< 2", 1) && !debug::hitConditionMet("<2", 2));
    bool valid = true;
    CHECK(debug::hitConditionMet("often", 1, &valid) && !valid);
}

} // namespace

int run_run_control_tests() {
    test_hit_condition_parsing();
    test_line_breakpoint_and_line_steps();
    test_step_over_a_call();
    test_conditions_hit_counts_and_logpoints();
    test_data_breakpoint_and_pause();
    test_bank_qualified_listing();
    std::printf("run control tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
