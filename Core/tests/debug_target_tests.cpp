// Headless tests for the debugger's machine layer: CPU breakpoints with
// skip-once resume, memory watches (data accesses only, never fetches),
// the machine debug targets (Core/Debug/MachineDebugTargets) and the
// expression evaluator (Core/Debug/DebugExpression). Same assert-and-tally
// style as lh5801_tests.cpp.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "../CPU/LH5801/LH5801.hpp"
#include "../CPU/SC7852/SC7852.hpp"
#include "../Debug/BreakpointTable.hpp"
#include "../Debug/CpuRegisters.hpp"
#include "../Debug/DebugExpression.hpp"
#include "../Debug/MachineDebugTargets.hpp"
#include "../PC1500/PC1500Machine.hpp"
#include "DebugTargetTestSupport.hpp"
#include "LegacyExpression.hpp"
#include "TestRoms.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

class LhBus : public LH5801Bus {
public:
    uint8_t mem[65536]{};
    uint8_t me1[65536]{};
    uint8_t readME0(uint16_t a) override { return mem[a]; }
    void    writeME0(uint16_t a, uint8_t v) override { mem[a] = v; }
    uint8_t readME1(uint16_t a) override { return me1[a]; }
    void    writeME1(uint16_t a, uint8_t v) override { me1[a] = v; }
};

class ZBus : public SC7852Bus {
public:
    uint8_t mem[65536]{};
    uint8_t readMem(uint16_t a) override { return mem[a]; }
    void    writeMem(uint16_t a, uint8_t v) override { mem[a] = v; }
    bool intLine = false;
    bool interruptLevel() const override { return intLine; }
};

// ── Expressions ───────────────────────────────────────────────────────────

debug::ExpressionResult eval(const std::string& text, bool bigEndian = false) {
    static std::map<std::string, int64_t> names = {{"a", 0x10}, {"hl", 0x7A00}, {"cf", 1}, {"LOOP", 0x40C5}};
    static uint8_t mem[65536] = {};
    mem[0x7A00] = 0x34; mem[0x7A01] = 0x12;
    debug::ExpressionContext ctx;
    ctx.lookup = [](const std::string& n, int64_t* v) {
        auto it = names.find(n);
        if (it == names.end()) return false;
        *v = it->second;
        return true;
    };
    ctx.readByte = [](uint16_t addr, bool me1, uint8_t* v) {
        if (me1) return false;
        *v = mem[addr];
        return true;
    };
    ctx.bigEndian = bigEndian;
    return debug::evaluate(text, ctx);
}

bool evalIs(const std::string& text, int64_t want, bool bigEndian = false) {
    debug::ExpressionResult r = eval(text, bigEndian);
    if (!r.ok || r.value != want)
        std::fprintf(stderr, "  expr \"%s\": ok=%d value=%lld error=%s\n", text.c_str(), r.ok, (long long)r.value, r.error.c_str());
    return r.ok && r.value == want;
}

void test_expressions() {
    CHECK(evalIs("A == 0x10", 1));
    CHECK(evalIs("a != 16", 0));
    CHECK(evalIs("1 + 2 * 3", 7));
    CHECK(evalIs("(1 + 2) * 3", 9));
    CHECK(evalIs("$FF & &0F | 0x100", 0x10F));
    CHECK(evalIs("1 << 4 >> 2", 4));
    CHECK(evalIs("-a + 20", 4));
    CHECK(evalIs("!cf || a >= 0x10 && a < 0x20", 1));
    CHECK(evalIs("~0 & 0xFF", 0xFF));
    CHECK(evalIs("[hl]", 0x34));
    CHECK(evalIs("w[hl]", 0x1234));
    CHECK(evalIs("w[hl]", 0x3412, /*bigEndian=*/true));
    CHECK(evalIs("[0x7A00 + 1] == 0x12", 1));
    CHECK(evalIs("LOOP", 0x40C5));
    CHECK(evalIs("10 % 4 + 9 / 2", 6));
    CHECK(!eval("nosuch + 1").ok);
    CHECK(!eval("#[0]").ok);      // unreadable
    CHECK(!eval("1 +").ok);
    CHECK(!eval("(1").ok);
    CHECK(!eval("1 / 0").ok);
    CHECK(!eval("").ok);
    CHECK(eval("nosuch").error.find("nosuch") != std::string::npos);
}

// ── CPU hooks ─────────────────────────────────────────────────────────────

void test_z80_breakpoint_and_skip_once() {
    ZBus bus;
    const uint8_t code[] = {0x00, 0x00, 0x18, 0xFC}; // nop; nop; jr 0x0000
    for (size_t i = 0; i < sizeof code; i++) bus.mem[i] = code[i];
    SC7852 cpu(bus);
    cpu.reset();
    cpu.addBreakpoint(0x0001);
    cpu.step();                 // no check until enabled
    cpu.step();
    CHECK(cpu.pc() == 0x0002);
    cpu.setBreakpointsEnabled(true);
    cpu.step();                 // jr 0x0000
    cpu.step();                 // nop at 0
    CHECK(cpu.step() == 0 && cpu.pc() == 0x0001);
    CHECK(cpu.consumeBreakpointHit());
    CHECK(cpu.step() == 0);     // still parked
    CHECK(cpu.consumeBreakpointHit());
    cpu.resumePastBreakpoint();
    CHECK(cpu.step() > 0 && cpu.pc() == 0x0002);
    cpu.step(); cpu.step();     // around the loop again
    CHECK(cpu.step() == 0 && cpu.consumeBreakpointHit()); // skip was once only
}

void test_z80_breakpoint_not_between_prefix_and_opcode() {
    ZBus bus;
    const uint8_t code[] = {0xDD, 0xFD, 0x21, 0x00, 0x00}; // DD carried, then FD 21
    for (size_t i = 0; i < sizeof code; i++) bus.mem[i] = code[i];
    SC7852 cpu(bus);
    cpu.reset();
    cpu.setBreakpointsEnabled(true);
    cpu.addBreakpoint(0x0001); // the carried FD's address
    cpu.step();                // DD: carries FD into the next step
    CHECK(cpu.step() > 0);     // mid-prefix: no breakpoint check
    CHECK(cpu.iy() == 0x0000 && cpu.pc() == 0x0005);
}

void test_z80_skip_is_tied_to_its_address() {
    // Parked on 0001 with an interrupt pending: the continue takes the
    // interrupt first. The handler's breakpoint still stops, and the skip
    // is still there for 0001 when the handler returns.
    ZBus bus;
    bus.mem[0x0000] = 0x00; bus.mem[0x0001] = 0x00; bus.mem[0x0002] = 0x00; // nop; nop; nop
    bus.mem[0x0038] = 0xC9;                                                  // ret
    SC7852 cpu(bus);
    cpu.reset();                 // IM 0, run as IM 1 (RST 38H)
    cpu.setBreakpointsEnabled(true);
    cpu.addBreakpoint(0x0001);
    cpu.addBreakpoint(0x0038);
    cpu.step();
    CHECK(cpu.step() == 0 && cpu.consumeBreakpointHit() && cpu.pc() == 0x0001);
    cpu.setIFF1(true);
    bus.intLine = true;
    cpu.resumePastBreakpoint();
    CHECK(cpu.step() > 0 && cpu.pc() == 0x0038); // interrupt entry
    bus.intLine = false;
    CHECK(cpu.step() == 0 && cpu.consumeBreakpointHit()); // not eaten by the skip
    cpu.removeBreakpoint(0x0038);
    CHECK(cpu.step() > 0 && cpu.pc() == 0x0001); // ret
    CHECK(cpu.step() > 0 && cpu.pc() == 0x0002); // the skip, spent where it was meant

    cpu.clearBreakpointSkip();   // and a cleared one is gone
    cpu.addBreakpoint(0x0002);
    CHECK(cpu.step() == 0 && cpu.consumeBreakpointHit());
}

void test_z80_breakpoint_keeps_ei_shadow() {
    // ei; nop (breakpoint) with INT pending: parking on the nop must not
    // drop the EI delay, so the nop still runs before the interrupt.
    ZBus bus;
    bus.mem[0x0000] = 0xFB; bus.mem[0x0001] = 0x00; bus.mem[0x0002] = 0x00; // ei; nop; nop
    SC7852 cpu(bus);
    cpu.reset();
    bus.intLine = true;
    cpu.setBreakpointsEnabled(true);
    cpu.addBreakpoint(0x0001);
    CHECK(cpu.step() > 0 && cpu.iff1());          // ei
    CHECK(cpu.step() == 0 && cpu.consumeBreakpointHit() && cpu.pc() == 0x0001);
    cpu.resumePastBreakpoint();
    CHECK(cpu.step() > 0 && cpu.pc() == 0x0002);  // the nop, not the interrupt
    CHECK(cpu.step() > 0 && cpu.pc() == 0x0038);  // now the interrupt
}

void test_lh5801_skip_once() {
    LhBus bus;
    bus.mem[0xFFFE] = 0x40; bus.mem[0xFFFF] = 0x00;
    bus.mem[0x4000] = 0x38; bus.mem[0x4001] = 0x9E; bus.mem[0x4002] = 0x03; // nop; bch 0x4000
    LH5801 cpu(bus);
    cpu.reset();
    cpu.setBreakpointsEnabled(true);
    cpu.addBreakpoint(0x4000);
    CHECK(cpu.step() == 0 && cpu.consumeBreakpointHit());
    cpu.resumePastBreakpoint();
    CHECK(cpu.step() > 0 && cpu.pc() == 0x4001);
    cpu.step();
    CHECK(cpu.pc() == 0x4000 && cpu.step() == 0);
}

void test_breakpoint_set() {
    BreakpointSet set;
    set.add(0x0000);
    set.add(0xFFFF);
    set.add(0x0040);
    CHECK(set.contains(0x0000) && set.contains(0xFFFF) && set.contains(0x0040));
    CHECK(!set.contains(0x0001) && !set.contains(0xFFFE) && !set.contains(0x003F) && !set.contains(0x0041));
    CHECK(!set.consumeHit() && !set.check(0x1234) && !set.consumeHit());
    CHECK(set.check(0xFFFF) && set.consumeHit() && !set.consumeHit());
    set.remove(0xFFFF);
    CHECK(!set.contains(0xFFFF) && set.contains(0x0000));
    set.clear();
    CHECK(!set.contains(0x0000) && !set.contains(0x0040));
}

void test_trace_flags_leave_breakpoints_alone() {
    // Trace flags alone never check breakpoints.
    LhBus bus;
    bus.mem[0xFFFE] = 0x40; bus.mem[0xFFFF] = 0x00;
    bus.mem[0x4000] = 0x38; bus.mem[0x4001] = 0x9E; bus.mem[0x4002] = 0x03; // nop; bch 0x4000
    LH5801 cpu(bus);
    cpu.reset();
    cpu.addBreakpoint(0x4000);
    cpu.setTraceFlags(TRACE_FULL);
    CHECK(cpu.step() > 0);
    // A trace capture starting and ending leaves the debugger's enable as it was.
    PC1500Machine machine;
    machine.cpu().setBreakpointsEnabled(true);
    std::FILE* f = std::tmpfile();
    CHECK(f && machine.beginCpuTrace(f, TRACE_FULL));
    CHECK(machine.cpu().breakpointsEnabled());
    machine.endCpuTrace();
    CHECK(machine.cpu().breakpointsEnabled() && machine.cpu().traceFlags() == TRACE_NONE);
}

void test_reset_clears_breakpoint_state() {
    LhBus bus;
    bus.mem[0xFFFE] = 0x40; bus.mem[0xFFFF] = 0x00;
    bus.mem[0x4000] = 0x38;
    LH5801 cpu(bus);
    cpu.reset();
    cpu.setBreakpointsEnabled(true);
    cpu.addBreakpoint(0x4000);
    CHECK(cpu.step() == 0);            // hit latched, not consumed
    cpu.resumePastBreakpoint();        // and a pending skip
    cpu.reset();
    CHECK(!cpu.consumeBreakpointHit());
    CHECK(cpu.step() == 0 && cpu.consumeBreakpointHit()); // the skip didn't survive

    ZBus zbus;
    SC7852 z(zbus);
    z.reset();
    z.setBreakpointsEnabled(true);
    z.addBreakpoint(0x0000);
    CHECK(z.step() == 0);
    z.resumePastBreakpoint();
    z.reset();
    CHECK(!z.consumeBreakpointHit());
    CHECK(z.step() == 0 && z.consumeBreakpointHit());
}

void test_lh5801_watches_skip_fetches() {
    LhBus bus;
    bus.mem[0xFFFE] = 0x40; bus.mem[0xFFFF] = 0x00;
    const uint8_t code[] = {
        0xB5, 0x5A,             // 4000 ldi a,0x5A
        0xA5, 0x40, 0x00,       // 4002 lda (0x4000)       -- a data read of code
        0xAE, 0x78, 0x10,       // 4005 sta (0x7810)
        0xFD, 0xAE, 0x78, 0x10, // 4008 sta #(0x7810)
    };
    for (size_t i = 0; i < sizeof code; i++) bus.mem[0x4000 + i] = code[i];
    LH5801 cpu(bus);
    cpu.reset();
    WatchSet w;
    w.add({0x4000, 0x4001, 0, true, false}); // read watch on the first instruction's bytes
    w.add({0x7810, 0x7810, 1, false, true}); // ME1 write watch
    cpu.setWatches(&w);
    cpu.step();                       // fetching 4000/4001 is not a data read
    CHECK(!w.hitPending());
    cpu.step();                       // lda (0x4000) is
    CHECK(w.hitPending());
    WatchHit h = w.consumeHit();
    CHECK(h.addr == 0x4000 && !h.write && h.value == 0xB5 && h.space == 0);
    cpu.step();                       // ME0 store: the watch is on ME1
    CHECK(!w.hitPending());
    cpu.step();
    CHECK(w.hitPending());
    h = w.consumeHit();
    CHECK(h.addr == 0x7810 && h.write && h.value == 0xB5 && h.space == 1);
    CHECK(bus.me1[0x7810] == 0xB5); // the instruction completed
    cpu.setWatches(nullptr);
}

void test_z80_watches() {
    ZBus bus;
    const uint8_t code[] = {0x3E, 0x77, 0x32, 0x00, 0x80, 0xC5}; // ld a,0x77; ld (0x8000),a; push bc
    for (size_t i = 0; i < sizeof code; i++) bus.mem[i] = code[i];
    SC7852 cpu(bus);
    cpu.reset();
    cpu.setSP(0x9000);
    WatchSet w;
    w.add({0x8000, 0x8000, 0, false, true});
    w.add({0x8FFE, 0x8FFF, 0, false, true}); // the stack push
    cpu.setWatches(&w);
    cpu.step();
    CHECK(!w.hitPending());
    cpu.step();
    CHECK(w.hitPending() && w.consumeHit().value == 0x77);
    cpu.step();
    CHECK(w.hitPending() && w.consumeHit().addr == 0x8FFF);
    cpu.setWatches(nullptr);
}

// ── Machine targets ───────────────────────────────────────────────────────

void test_pc1500_target() {
    PC1500Machine machine;
    if (!machine.loadROMFile("roms/PC-1500_A04.ROM")) {
        std::printf("  SKIP test_pc1500_target: roms/PC-1500_A04.ROM missing\n");
        return;
    }
    machine.reset();
    debug::PC1500DebugTarget target(machine);
    CHECK(target.threads().size() == 1 && target.busOwner() == 1);
    CHECK(target.pc(1) == 0xE000);
    CHECK(target.decode(1, 0xE000).text == "rie");

    // A breakpoint on the boot's input-buffer clear loop (D0B0).
    target.setBreakpoints(1, {0xD0B0});
    debug::Stop s = debugtest::runFrom(target, 2000000);
    CHECK(s.kind == debug::Stop::Breakpoint && s.thread == 1);
    CHECK(target.pc(1) == 0xD0B0);
    CHECK(target.historySize(1) >= 20);
    CHECK(target.history(1, 0).len > 0);
    // Resuming leaves the breakpoint; it's a loop, so it comes back.
    s = debugtest::runFrom(target, 2000000);
    CHECK(s.kind == debug::Stop::Breakpoint && target.pc(1) == 0xD0B0);
    target.setBreakpoints(1, {});
    CHECK(!target.breakpointsActive());
    CHECK(!machine.cpu().breakpointsEnabled());

    // Step one instruction.
    const uint32_t before = target.retired(1);
    s = debugtest::stepInstruction(target, 1, 100);
    CHECK(s.kind == debug::Stop::None && target.retired(1) == before + 1);

    // Registers and expressions.
    CHECK(target.writeRegister(1, "a", 0x42));
    uint32_t v = 0;
    CHECK(target.readRegister(1, "a", &v) && v == 0x42);
    CHECK(target.writeRegister(1, "xh", 0x12) && target.readRegister(1, "x", &v) && (v >> 8) == 0x12);
    CHECK(target.writeRegister(1, "cf", 1) && target.readRegister(1, "t", &v) && (v & 1));
    debug::ExpressionResult r = debug::evaluate("a == 0x42 && cf", target.expressionContext(1));
    CHECK(r.ok && r.value == 1);

    // Memory: RAM round trip; ME1's I/O chip reads as its latches; the ROM refuses.
    CHECK(target.poke(1, debug::kSpaceMain, 0x7A00, 0x99));
    uint8_t b = 0;
    CHECK(target.peek(1, debug::kSpaceMain, 0x7A00, &b) && b == 0x99);
    CHECK(!target.poke(1, debug::kSpaceMain, 0xC000, 0x00));
    CHECK(!target.poke(1, debug::kSpaceME1, 0x7A00, 0x00));

    // A write watch on the RAM the ROM keeps updating while idle stops
    // right after the writing instruction.
    target.setWatches(1, {{0x7600, 0x7BFF, 0, false, true}});
    s = debugtest::runFrom(target, 20000000);
    CHECK(s.kind == debug::Stop::Watch && s.thread == 1 && s.hit.write);
    CHECK(s.hit.addr >= 0x7600 && s.hit.addr <= 0x7BFF);
    target.setWatches(1, {});
}

void test_pc1600_target() {
    PC1600Machine machine;
    if (!bootPC1600(machine)) {
        std::printf("  SKIP test_pc1600_target: PC-1600 ROMs missing\n");
        return;
    }
    debug::PC1600DebugTarget target(machine);
    CHECK(target.threads().size() == 2);
    CHECK(target.busOwner() == debug::PC1600DebugTarget::kZ80);
    CHECK(target.historySize(1) > 0);
    CHECK(target.bankAt(1, target.pc(1)) >= 0);

    // The BASIC command loop spins; a breakpoint on the current PC stops
    // the next pass, and a resume comes back to it.
    const uint16_t pc = target.pc(1);
    target.setBreakpoints(1, {pc});
    debug::Stop s = debugtest::runFrom(target, 2000000);
    CHECK(s.kind == debug::Stop::Breakpoint && s.thread == 1 && target.pc(1) == pc);
    s = debugtest::runFrom(target, 2000000);
    CHECK(s.kind == debug::Stop::Breakpoint && target.pc(1) == pc);
    target.setBreakpoints(1, {});
    CHECK(!machine.sc7852().breakpointsEnabled());

    const uint32_t before = target.retired(1);
    s = debugtest::stepInstruction(target, 1, 100);
    CHECK(s.kind == debug::Stop::None && target.retired(1) == before + 1);

    uint32_t v = 0;
    CHECK(target.writeRegister(1, "bc'", 0xBEEF) && target.readRegister(1, "bc2", &v) && v == 0xBEEF);
    CHECK(target.writeRegister(1, "h", 0x12) && target.readRegister(1, "hl", &v) && (v >> 8) == 0x12);
    CHECK(target.readRegister(1, "zf", &v));

    // Z-80 RAM (page D, internal) round trip through the host path.
    uint8_t b = 0;
    CHECK(target.poke(1, debug::kSpaceMain, 0xC200, 0x5A) && target.peek(1, debug::kSpaceMain, 0xC200, &b) && b == 0x5A);
    // The LH5803 sees Z-80 8000-FFFF at its 0000-7FFF.
    CHECK(target.peek(2, debug::kSpaceMain, 0x4200, &b) && b == 0x5A);
    // Its UART block on ME1 can't be peeked without side effects.
    CHECK(!target.peek(2, debug::kSpaceME1, 0xA020, &b));
    CHECK(target.decode(1, 0x0000).len >= 1);
}

void test_pc1600_lh5803_thread() {
    // Z-80: OUT (38H),A ; HALT hands the bus to the LH5803, whose ROM loops
    // at C000: sta (0x1000) (Z-80 0x9000) ; nop ; bch C000.
    PC1600Machine m;
    std::vector<uint8_t> lower(PC1600Memory::kBankSize, 0x00), upper(PC1600Memory::kBankSize, 0x00);
    lower[0] = 0xD3; lower[1] = 0x38; lower[2] = 0x76;
    CHECK(m.loadBank0(lower.data(), lower.size(), upper.data(), upper.size()));
    std::vector<uint8_t> rom(16384, 0x00);
    const uint8_t code[] = {0xAE, 0x10, 0x00, 0x38, 0x9E, 0x06};
    for (size_t i = 0; i < sizeof code; i++) rom[i] = code[i];
    rom[16384 - 2] = 0xC0; rom[16384 - 1] = 0x00;
    CHECK(m.loadLH5803Rom(rom.data(), rom.size()));
    m.reset();

    debug::PC1600DebugTarget target(m);
    target.setBreakpoints(debug::PC1600DebugTarget::kLh5803, {0xC003});
    debug::Stop s = debugtest::runFrom(target, 100000);
    CHECK(s.kind == debug::Stop::Breakpoint && s.thread == debug::PC1600DebugTarget::kLh5803);
    CHECK(target.busOwner() == debug::PC1600DebugTarget::kLh5803 && target.pc(2) == 0xC003);
    CHECK(target.decode(2, 0xC000).text == "sta (0x1000)");
    // Resumes past it and comes round again.
    s = debugtest::runFrom(target, 100000);
    CHECK(s.kind == debug::Stop::Breakpoint && target.pc(2) == 0xC003);
    target.setBreakpoints(debug::PC1600DebugTarget::kLh5803, {});

    // A write watch on the LH5803's 0x1000 fires; the same address on the
    // Z-80 side is a different place and doesn't.
    target.setWatches(debug::PC1600DebugTarget::kZ80, {{0x1000, 0x1000, 0, false, true}});
    s = debugtest::runFrom(target, 1000);
    CHECK(s.kind == debug::Stop::None);
    target.setWatches(debug::PC1600DebugTarget::kZ80, {});
    target.setWatches(debug::PC1600DebugTarget::kLh5803, {{0x1000, 0x1000, 0, false, true}});
    s = debugtest::runFrom(target, 100000);
    CHECK(s.kind == debug::Stop::Watch && s.thread == debug::PC1600DebugTarget::kLh5803 && s.hit.addr == 0x1000);
    CHECK(target.pc(2) == 0xC003); // stopped after the store
    // Stepping reports the watch too.
    s = debugtest::stepInstruction(target, 2, 10); // nop
    CHECK(s.kind == debug::Stop::None);
    s = debugtest::stepInstruction(target, 2, 10); // bch
    s = debugtest::stepInstruction(target, 2, 10); // sta
    CHECK(s.kind == debug::Stop::Watch && s.thread == debug::PC1600DebugTarget::kLh5803);
}

void test_pc1600_skip_only_for_the_running_cpu() {
    // Same machine as test_pc1600_lh5803_thread(). The LH5803 sits at its
    // reset address C000 off the bus while the Z-80 runs; a continue must
    // not hand it the skip, so it stops at C000 when it gets the bus.
    PC1600Machine m;
    std::vector<uint8_t> lower(PC1600Memory::kBankSize, 0x00), upper(PC1600Memory::kBankSize, 0x00);
    lower[0] = 0xD3; lower[1] = 0x38; lower[2] = 0x76;
    CHECK(m.loadBank0(lower.data(), lower.size(), upper.data(), upper.size()));
    std::vector<uint8_t> rom(16384, 0x00);
    const uint8_t code[] = {0xAE, 0x10, 0x00, 0x38, 0x9E, 0x06};
    for (size_t i = 0; i < sizeof code; i++) rom[i] = code[i];
    rom[16384 - 2] = 0xC0; rom[16384 - 1] = 0x00;
    CHECK(m.loadLH5803Rom(rom.data(), rom.size()));
    m.reset();

    debug::PC1600DebugTarget target(m);
    CHECK(target.busOwner() == debug::PC1600DebugTarget::kZ80 && target.pc(2) == 0xC000);
    target.setBreakpoints(debug::PC1600DebugTarget::kLh5803, {0xC000});
    const uint32_t before = target.retired(2);
    debug::Stop s = debugtest::runFrom(target, 100000);
    CHECK(s.kind == debug::Stop::Breakpoint && s.thread == debug::PC1600DebugTarget::kLh5803);
    CHECK(target.pc(2) == 0xC000 && target.retired(2) == before); // its first instruction, not a later pass
}

void test_machine_step_latches_stops() {
    // PC-1500: a stepped store into a watched byte latches a watch stop on
    // CPU 1; a reset drops a stop nobody consumed.
    {
        PC1500Machine m;
        WatchSet w;
        w.add({0x4100, 0x4100, 0, false, true});
        m.setWatches(1, &w);
        const uint8_t code[] = {0xAE, 0x41, 0x00}; // sta (0x4100)
        for (size_t i = 0; i < sizeof code; i++) m.memory().poke(uint16_t(0x4000 + i), code[i]);
        m.cpu().setPC(0x4000);
        m.step();
        const DebugStop s = m.consumeDebugStop();
        CHECK(s.kind == DebugStop::Watch && s.cpu == 1);
        CHECK(m.consumeDebugStop().kind == DebugStop::None);
        m.cpu().setPC(0x4000);
        w.consumeHit();
        m.step();
        m.reset();
        CHECK(m.consumeDebugStop().kind == DebugStop::None);
        m.setWatches(1, nullptr);
    }
    // PC-1600: the Z-80's store latches CPU 1; after the handoff the
    // LH5803's store latches CPU 2 -- from step(), not only runCycles().
    {
        PC1600Machine m;
        std::vector<uint8_t> lower(PC1600Memory::kBankSize, 0x00), upper(PC1600Memory::kBankSize, 0x00);
        const uint8_t z80[] = {0x32, 0x00, 0x90, 0xD3, 0x38, 0x76}; // ld (0x9000),a ; out (38h),a ; halt
        for (size_t i = 0; i < sizeof z80; i++) lower[i] = z80[i];
        CHECK(m.loadBank0(lower.data(), lower.size(), upper.data(), upper.size()));
        std::vector<uint8_t> rom(16384, 0x00);
        const uint8_t lh[] = {0xAE, 0x10, 0x00, 0x9E, 0x05}; // sta (0x1000) ; bch C000
        for (size_t i = 0; i < sizeof lh; i++) rom[i] = lh[i];
        rom[16384 - 2] = 0xC0; rom[16384 - 1] = 0x00;
        CHECK(m.loadLH5803Rom(rom.data(), rom.size()));
        m.reset();
        WatchSet z, l;
        z.add({0x9000, 0x9000, 0, false, true});
        l.add({0x1000, 0x1000, 0, false, true});
        m.setWatches(1, &z);
        m.setWatches(2, &l);
        m.step();
        DebugStop s = m.consumeDebugStop();
        CHECK(s.kind == DebugStop::Watch && s.cpu == 1);
        z.consumeHit();
        s = {};
        for (int i = 0; i < 50 && s.kind == DebugStop::None; i++) {
            m.step();
            s = m.consumeDebugStop();
        }
        CHECK(s.kind == DebugStop::Watch && s.cpu == 2);
        m.setWatches(1, nullptr);
        m.setWatches(2, nullptr);
    }
}

void test_cpu_views() {
    PC1600Machine m;
    std::vector<uint8_t> lower(PC1600Memory::kBankSize, 0x76), upper(PC1600Memory::kBankSize, 0x00); // halt
    CHECK(m.loadBank0(lower.data(), lower.size(), upper.data(), upper.size()));
    std::vector<uint8_t> rom(16384, 0x38);
    rom[16384 - 2] = 0xC0; rom[16384 - 1] = 0x00;
    CHECK(m.loadLH5803Rom(rom.data(), rom.size()));
    m.reset();
    {
        debug::PC1600DebugTarget target(m);
        const auto threads = target.threads();
        CHECK(threads.size() == 2 && threads[0].kind == debug::CpuKind::Z80 && threads[1].name == "LH5803");
        // A thread id beyond the CPUs names the last one.
        CHECK(target.kindOf(7) == debug::CpuKind::LH5803 && target.pc(7) == target.pc(2));
        // An edited PV reaches the LH5803's bus at once.
        CHECK(target.writeRegister(2, "pv", 1) && m.lh5803Memory().pv());
        CHECK(target.writeRegister(2, "pv", 0) && !m.lh5803Memory().pv());
        // The flags byte comes from the register marked as status, live
        // and in history; the bank state is the machine's own.
        uint8_t flags = 0;
        CHECK(debug::statusFlags(target.registers(1), &flags) && flags == uint8_t(m.sc7852().af()));
        CHECK(debug::statusFlags(target.registers(2), &flags) && flags == m.lh5803().statusReg());
        const auto z80Banks = target.bankState(1);
        CHECK(z80Banks.size() == 4 && z80Banks[1].name == "Page B (4000-7FFF)" && z80Banks[1].value.rfind("bank ", 0) == 0);
        const auto lhBanks = target.bankState(2);
        CHECK(lhBanks.size() == 2 && lhBanks[0].name == "PU" && lhBanks[1].name == "PV");
        // Halted: the Z-80 after HALT; the LH580x also when powered off.
        m.step();
        CHECK(target.historySize(1) > 0 && debug::statusFlags(target.history(1, 0).registers, &flags));
        CHECK(target.halted(1));
        CHECK(!target.halted(2));
        target.setBreakpoints(1, {0x0000});
        target.setBreakpoints(2, {0xC000});
        target.setWatches(2, {{0x1000, 0x1000, 0, false, true}});
        CHECK(m.sc7852().breakpointsEnabled() && m.lh5803().breakpointsEnabled());
    }
    // The target took everything off the CPUs.
    CHECK(!m.sc7852().breakpointsEnabled() && !m.lh5803().breakpointsEnabled());
    m.lh5803().setBreakpointsEnabled(true);
    m.lh5803().setPC(0xC000);
    CHECK(m.lh5803().step() > 0); // no breakpoint left at C000
    m.lh5803().setBreakpointsEnabled(false);
}

void test_register_reads() {
    // Every listed register reads back its listed value, plus the halves
    // and aliases the list doesn't show.
    LhBus bus;
    LH5801 lh(bus);
    lh.reset();
    lh.setX(0x1234); lh.setY(0x5678); lh.setU(0x9ABC); lh.setA(0x42); lh.setStatusReg(0x15); lh.setPV(true);
    uint32_t v = 0;
    for (const debug::Register& r : debug::lhRegisters(lh))
        CHECK(debug::lhReadRegister(lh, r.name, &v) && v == r.value);
    CHECK(debug::lhReadRegister(lh, "xh", &v) && v == 0x12 && debug::lhReadRegister(lh, "ul", &v) && v == 0xBC);
    CHECK(debug::lhReadRegister(lh, "zf", &v) && !debug::lhReadRegister(lh, "q", &v));

    ZBus zbus;
    SC7852 z(zbus);
    z.reset();
    z.setAF(0x1234); z.setBC(0x5678); z.setIX(0xABCD); z.setAF2(0x4321); z.setI(0x3F);
    for (const debug::Register& r : debug::z80Registers(z))
        CHECK(debug::z80ReadRegister(z, r.name, &v) && v == r.value);
    CHECK(debug::z80ReadRegister(z, "af2", &v) && v == 0x4321);
    CHECK(debug::z80ReadRegister(z, "a", &v) && v == 0x12 && debug::z80ReadRegister(z, "f", &v) && v == 0x34);
    CHECK(debug::z80ReadRegister(z, "ixl", &v) && v == 0xCD && debug::z80ReadRegister(z, "c", &v) && v == 0x78);
    CHECK(debug::z80ReadRegister(z, "zf", &v) && !debug::z80ReadRegister(z, "q", &v));
}

void test_watch_set() {
    WatchSet w;
    w.add({0x0000, 0x0001, 0, true, false});   // read, ME0
    w.add({0xFFFF, 0xFFFF, 1, false, true});   // write, ME1
    w.add({0xFFF0, 0x000F, 0, true, true});    // wrapped: never matches
    w.check(0x0002, 0, false, 0);
    CHECK(!w.hitPending());
    w.check(0x0001, 0, true, 0);               // a write where only reads are watched
    CHECK(!w.hitPending());
    w.check(0xFFFF, 0, true, 0);               // the right address in the wrong space
    w.check(0xFFF5, 0, false, 0);              // inside the wrapped range
    CHECK(!w.hitPending());
    w.check(0x0001, 0x5A, false, 0);
    w.check(0xFFFF, 0x77, true, 1);            // later hits don't replace the first
    CHECK(w.hitPending());
    const WatchHit hit = w.consumeHit();
    CHECK(hit.addr == 0x0001 && hit.value == 0x5A && !hit.write && hit.space == 0 && !w.hitPending());
    w.check(0xFFFF, 0x77, true, 1);
    CHECK(w.hitPending() && w.consumeHit().space == 1);
    w.clear();
    w.check(0x0001, 0, false, 0);
    CHECK(!w.hitPending() && w.empty());
}

void test_compiled_expressions_match_legacy() {
    static std::map<std::string, int64_t> names = {{"a", 0x10}, {"hl", 0x7A00}, {"cf", 1}, {"LOOP", 0x40C5}, {"Mixed", 7}};
    static uint8_t mem[65536] = {};
    mem[0x7A00] = 0x34; mem[0x7A01] = 0x12; mem[0x0010] = 0x99;
    debug::ExpressionContext full;
    full.lookup = [](const std::string& n, int64_t* v) {
        auto it = names.find(n);
        if (it == names.end()) return false;
        *v = it->second;
        return true;
    };
    full.readByte = [](uint16_t addr, bool me1, uint8_t* v) {
        if (addr == 0x1234 || (me1 && addr >= 0x8000)) return false;
        *v = mem[addr];
        return true;
    };
    debug::ExpressionContext bigEndian = full;
    bigEndian.bigEndian = true;
    debug::ExpressionContext noMemory = full;
    noMemory.readByte = nullptr;
    debug::ExpressionContext nothing;
    const debug::ExpressionContext* contexts[] = {&full, &bigEndian, &noMemory, &nothing};

    int mismatches = 0;
    auto same = [&](const std::string& text) {
        for (const debug::ExpressionContext* ctx : contexts) {
            const debug::ExpressionResult want = legacy::evaluate(text, *ctx);
            const debug::ExpressionResult got = debug::evaluate(text, *ctx);
            if (want.ok != got.ok || want.error != got.error || (want.ok && want.value != got.value)) {
                if (mismatches++ < 10)
                    std::fprintf(stderr, "  expr \"%s\": legacy ok=%d %lld '%s', compiled ok=%d %lld '%s'\n", text.c_str(),
                                 want.ok, (long long)want.value, want.error.c_str(), got.ok, (long long)got.value,
                                 got.error.c_str());
            }
        }
    };
    const char* const corpus[] = {
        "", "  ", "1", "0x10", "$ff", "&7A00", "0x", "$", "12+3*4", "(1+2)*3", "-5 % 3", "~0", "!0", "!!7", "+3",
        "1<<4", "256>>2", "1<2", "2<=2", "3>2", "3>=4", "1==1", "1!=1", "6&3", "6|3", "6^3", "1&&0", "0||2",
        "a", "A", "hl", "Mixed", "mixed", "LOOP", "loop", "foo", "foo +", "foo bar", "a b", "1 2", "1/0", "1/0*",
        "1%0", "1/foo", "foo/0", "[hl]", "w[hl]", "#[0x10]", "#[0x9000]", "[0x1234]", "w[0x1233]", "[hl", "w[",
        "#[", "(1+2", "1+2)", "1+", "*2", "!=3", "a == 0x10 && cf", "[hl] == 0x34 || foo", "(foo", "[foo]",
        "[1/0]", "1 + [0x1234] + foo", "foo + [0x1234]", "0x7FFFFFFFFFFFFFFF+1", "-(-9223372036854775807-1)",
        "1<<64", "1<<63", "-1>>1", "a&&&cf", "a|||cf", "a<<<1", "a===1", "_x.y$'", "w [hl]", "W[hl]", "#(1)",
        "((((((1))))))", "1 ? 2", "@", "a @", "[hl] @", "1 +\t2",
    };
    for (const char* text : corpus) same(text);

    // Seeded random token soup: every mixture of valid and broken input.
    const char* const tokens[] = {"a", "hl", "foo", "LOOP", "Mixed", "0x10", "$ff", "&7A00", "12", "0", "[", "]", "w[",
                                  "#[", "(", ")", "+", "-", "*", "/", "%", "<<", ">>", "<", "<=", ">", ">=", "==",
                                  "!=", "&&", "||", "&", "|", "^", "!", "~", " ", "0x", "$", "0x1234", "@"};
    const size_t kTokens = sizeof tokens / sizeof tokens[0];
    uint32_t seed = 12345;
    auto next = [&seed] { seed = seed * 1103515245u + 12345u; return (seed >> 16) & 0x7FFF; };
    for (int i = 0; i < 20000; i++) {
        std::string text;
        const int n = 1 + int(next() % 9);
        for (int k = 0; k < n; k++) text += tokens[next() % kTokens];
        same(text);
    }
    CHECK(mismatches == 0);
}

void test_hit_conditions_and_log_templates_match_legacy() {
    const char* const hits[] = {"", "  ", "5", "== 5", "=5", ">= 3", "<=2", "!= 4", "> 1", "<3", "% 3", "%0", "% -2",
                                "0x10", ">= 0x2", "abc", "5x", "==", " % ", "-1", "= 3 "};
    int mismatches = 0;
    for (const char* h : hits)
        for (int n = 0; n <= 20; n++) {
            bool wantValid = false, gotValid = false;
            const bool want = legacy::hitConditionMet(h, n, &wantValid);
            const bool got = debug::hitConditionMet(h, n, &gotValid);
            if (want != got || wantValid != gotValid) {
                if (mismatches++ < 5) std::fprintf(stderr, "  hit \"%s\" at %d: legacy %d/%d, now %d/%d\n", h, n, want, wantValid, got, gotValid);
            }
        }
    debug::ExpressionContext ctx;
    ctx.lookup = [](const std::string& n, int64_t* v) {
        if (n != "a" && n != "x") return false;
        *v = n == "a" ? 0x42 : 0x1234;
        return true;
    };
    ctx.readByte = [](uint16_t addr, bool, uint8_t* v) { *v = uint8_t(addr); return addr != 7; };
    const char* const logs[] = {"", "plain", "a={a}", "{a}{x}", "x={x} a={a}!", "{foo}", "{[7]}", "{[8]}",
                                "{-1}", "{1/0}", "open {a", "}{a}", "{}", "{{a}}", "a={a} b={", "{ a + 1 }"};
    for (const char* m : logs)
        if (legacy::interpolateLog(m, ctx) != debug::interpolateLog(m, ctx)) {
            if (mismatches++ < 10)
                std::fprintf(stderr, "  log \"%s\": legacy '%s', now '%s'\n", m, legacy::interpolateLog(m, ctx).c_str(),
                             debug::interpolateLog(m, ctx).c_str());
        }
    CHECK(mismatches == 0);
}

void test_function_breakpoint_on_lh5803_symbol() {
    // A function breakpoint arms on the CPU whose binding defines the
    // symbol: here the LH5803 (thread 2).
    PC1600Machine m;
    std::vector<uint8_t> lower(PC1600Memory::kBankSize, 0x00), upper(PC1600Memory::kBankSize, 0x00);
    lower[0] = 0xD3; lower[1] = 0x38; lower[2] = 0x76; // out (38h),a ; halt
    CHECK(m.loadBank0(lower.data(), lower.size(), upper.data(), upper.size()));
    std::vector<uint8_t> rom(16384, 0x00);
    const uint8_t code[] = {0xAE, 0x10, 0x00, 0x38, 0x9E, 0x06}; // sta (0x1000) ; nop ; bch C000
    for (size_t i = 0; i < sizeof code; i++) rom[i] = code[i];
    rom[16384 - 2] = 0xC0; rom[16384 - 1] = 0x00;
    CHECK(m.loadLH5803Rom(rom.data(), rom.size()));
    m.reset();

    debug::PC1600DebugTarget target(m);
    debug::SourceMap map;
    debug::Listing symbols;
    symbols.symbols["LOOP"] = 0xC003;
    map.addStatic(debug::PC1600DebugTarget::kLh5803, symbols, {}, "lh.sym");
    debug::BreakpointTable table;
    debug::BreakpointTable::FunctionRequest r;
    r.name = "LOOP";
    const auto statuses = table.setFunctions({r}, map);
    CHECK(statuses.size() == 1 && statuses[0].verified && statuses[0].thread == 2 && statuses[0].addr == 0xC003);
    table.apply(target);
    const debug::Stop s = debugtest::runFrom(target, 100000);
    CHECK(s.kind == debug::Stop::Breakpoint && s.thread == 2 && target.pc(2) == 0xC003);
}

} // namespace

int run_debug_target_tests() {
    test_expressions();
    test_z80_breakpoint_and_skip_once();
    test_z80_breakpoint_not_between_prefix_and_opcode();
    test_z80_skip_is_tied_to_its_address();
    test_z80_breakpoint_keeps_ei_shadow();
    test_lh5801_skip_once();
    test_breakpoint_set();
    test_trace_flags_leave_breakpoints_alone();
    test_reset_clears_breakpoint_state();
    test_lh5801_watches_skip_fetches();
    test_z80_watches();
    test_pc1500_target();
    test_pc1600_target();
    test_pc1600_lh5803_thread();
    test_pc1600_skip_only_for_the_running_cpu();
    test_machine_step_latches_stops();
    test_cpu_views();
    test_register_reads();
    test_watch_set();
    test_compiled_expressions_match_legacy();
    test_hit_conditions_and_log_templates_match_legacy();
    test_function_breakpoint_on_lh5803_symbol();
    std::printf("debug target tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
