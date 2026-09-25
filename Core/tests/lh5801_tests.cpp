// Headless C++ test suite for the LH5801 core + PC1500Memory/PC1500Machine.
// No external test framework dependency; plain assert-style checks with a
// running pass/fail tally (opcode-level tests, a boot smoke test,
// trace-ring/breakpoint tests).
//
// Build & run: see tools/run_tests.sh

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../CPU/LH5801/LH5801.hpp"
#include "../PC1500/PC1500Display.hpp"
#include "../PC1500/PC1500Keyboard.hpp"
#include "../PC1500/PC1500Machine.hpp"
#include "../PC1500/PC1500Memory.hpp"
#include "../PC1500/PC1500PresetLoader.hpp"
#include "PresetTestSupport.hpp"

#include <ctime>

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// A trivial RAM-backed bus for isolated opcode tests: both ME0 and ME1 map
// to the same flat 64KB array, so tests can freely poke code/data anywhere
// without worrying about PC1500Memory's real address-decode holes.
class FlatBus : public LH5801Bus {
public:
    uint8_t mem[65536]{};
    uint8_t readME0(uint16_t addr) override { return mem[addr]; }
    void    writeME0(uint16_t addr, uint8_t v) override { mem[addr] = v; }
    uint8_t readME1(uint16_t addr) override { return mem[addr]; }
    void    writeME1(uint16_t addr, uint8_t v) override { mem[addr] = v; }
};

// Loads `code` at 0x8000, sets the reset vector to point there, resets, and
// returns a ready-to-step CPU. `code` may contain multiple instructions;
// tests call step() as many times as needed.
struct Rig {
    FlatBus bus;
    LH5801 cpu;
    explicit Rig(std::vector<uint8_t> code) : cpu(bus) {
        for (size_t i = 0; i < code.size(); i++) bus.mem[0x8000 + i] = code[i];
        bus.mem[0xFFFE] = 0x80;
        bus.mem[0xFFFF] = 0x00;
        cpu.reset();
    }
};

void test_reset_vector() {
    FlatBus bus;
    bus.mem[0xFFFE] = 0x12;
    bus.mem[0xFFFF] = 0x34;
    LH5801 cpu(bus);
    CHECK(cpu.pc() == 0x1234);
    CHECK(cpu.statusReg() == 0);
    CHECK(!cpu.flagC() && !cpu.flagIE() && !cpu.flagZ());
}

void test_ldi_a_and_zero_flag() {
    Rig r({0xB5, 0x00, 0xB5, 0x7F}); // ldi a,0x00 ; ldi a,0x7F
    r.cpu.step();
    CHECK(r.cpu.a() == 0x00);
    CHECK(r.cpu.flagZ());
    r.cpu.step();
    CHECK(r.cpu.a() == 0x7F);
    CHECK(!r.cpu.flagZ());
}

void test_adc_carry_and_overflow() {
    // ldi a,0xFF ; sec ; adc ul  (ul defaults to 0) => a = 0xFF+0+1 = 0x00, C=1,Z=1
    Rig r({0xB5, 0xFF, 0xFB, 0x22});
    r.cpu.step(); // ldi a,0xFF
    r.cpu.step(); // sec
    CHECK(r.cpu.flagC());
    r.cpu.step(); // adc ul
    CHECK(r.cpu.a() == 0x00);
    CHECK(r.cpu.flagC());  // carry out of bit 7
    CHECK(r.cpu.flagZ());
}

void test_adc_signed_overflow_v_flag() {
    // ldi a,0x7F ; rec ; ldi xl,0x01 ; adc xl => 0x7F+1 = 0x80: V should set (pos+pos=neg)
    Rig r({0xB5, 0x7F, 0xF9, 0x4A, 0x01, 0x02});
    r.cpu.step(); r.cpu.step(); r.cpu.step();
    r.cpu.step(); // adc xl
    CHECK(r.cpu.a() == 0x80);
    CHECK(r.cpu.flagV());
    CHECK(!r.cpu.flagC());
}

void test_sbc_no_borrow_convention() {
    // ldi a,0x05 ; sec ; ldi xl,0x03 ; sbc xl => 5-3-0=2, C=1 (no borrow)
    Rig r({0xB5, 0x05, 0xFB, 0x4A, 0x03, 0x00});
    r.cpu.step(); r.cpu.step(); r.cpu.step();
    r.cpu.step(); // sbc xl
    CHECK(r.cpu.a() == 0x02);
    CHECK(r.cpu.flagC()); // no borrow: A >= operand

    // ldi a,0x03 ; sec ; ldi xl,0x05 ; sbc xl => 3-5 = -2 (0xFE), C=0 (borrow)
    Rig r2({0xB5, 0x03, 0xFB, 0x4A, 0x05, 0x00});
    r2.cpu.step(); r2.cpu.step(); r2.cpu.step();
    r2.cpu.step();
    CHECK(r2.cpu.a() == 0xFE);
    CHECK(!r2.cpu.flagC()); // borrow occurred
}

void test_cpa_flags_no_writeback() {
    // ldi a,0x05 ; sec ; ldi xl,0x05 ; cpa xl => equal: C=1,Z=1, A unmodified
    Rig r({0xB5, 0x05, 0xFB, 0x4A, 0x05, 0x06});
    r.cpu.step(); r.cpu.step(); r.cpu.step();
    r.cpu.step(); // cpa xl
    CHECK(r.cpu.a() == 0x05); // A not modified by compare
    CHECK(r.cpu.flagC());
    CHECK(r.cpu.flagZ());
}

void test_lop_iterates_n_plus_1_times() {
    // ldi ul,0x02 ; lop ul,+2 (a 2-byte self-loop -- e=2 branches back to
    // the lop opcode's own address; this is the exact byte pattern "88 02"
    // seen in the real PC-1500_A04.ROM's own reset-delay loop at 0xE018).
    // Per guide: ldi ul,N loops N+1 times. N=2 -> 3 iterations of the lop
    // instruction itself (UL: 2->1->0->0xFF, first two are "no borrow"
    // branches, third is the fall-through).
    Rig r({0x6A, 0x02, 0x88, 0x02});
    r.cpu.step(); // ldi ul,2
    CHECK(r.cpu.ul() == 2);
    r.cpu.step(); // lop: UL 2->1, no borrow, branch (P back to the lop opcode)
    CHECK(r.cpu.ul() == 1);
    CHECK(r.cpu.pc() == 0x8002);
    r.cpu.step(); // lop: UL 1->0, no borrow, branch
    CHECK(r.cpu.ul() == 0);
    CHECK(r.cpu.pc() == 0x8002);
    r.cpu.step(); // lop: UL 0->0xFF, borrow, fall through
    CHECK(r.cpu.ul() == 0xFF);
    CHECK(r.cpu.pc() == 0x8004);
}

void test_inc_dec_16bit_no_flags() {
    // sec ; ldi xh,0x00 ; ldi xl,0xFF ; inc x -> x=0x0100, flags untouched by inc x
    Rig r({0xFB, 0x48, 0x00, 0x4A, 0xFF, 0x44});
    r.cpu.step(); // sec
    r.cpu.step(); r.cpu.step(); // ldi xh/xl
    CHECK(r.cpu.x() == 0x00FF);
    r.cpu.step(); // inc x
    CHECK(r.cpu.x() == 0x0100);
    CHECK(r.cpu.flagC()); // untouched from the earlier sec
}

void test_adr_preserves_flags() {
    // ADR (FD CA/DA/EA) must leave C/V/H/Z *unchanged* -- the low-byte
    // addition's carry propagates into RH internally but is never published
    // to the status register. The ROM's key-dispatch path depends on carry
    // surviving an `adr y` at 0xD2AC across the `rtn` at 0xD2AE to reach the
    // `bcr` at 0xDCA0; clobbering it breaks PRO-mode Up/Down line-listing
    // redraw and the RUN-mode BREAK "peek current line" (display RAM never
    // updated, though the ROM's own text buffer does). See LH5801.cpp's ADR
    // case.
    //
    // This behavior contradicts the PC-1500 TRM's own text (which says ADR
    // does change C/H/Z/V, and backs it with a worked example), but is
    // confirmed on real hardware (PC-1500A) via
    // examples/debug/adrtest_1500a.asm.

    // sec ; ldi a,0x00 ; ldi yh,0x7B ; ldi yl,0xB2 ; adr y
    // Adding 0 produces no carry out, so a flag-clobbering ADR would clear
    // the carry the `sec` just set -- exactly the real failure.
    Rig r({0xFB, 0xB5, 0x00, 0x58, 0x7B, 0x5A, 0xB2, 0xFD, 0xDA});
    r.cpu.step(); // sec
    r.cpu.step(); // ldi a,0x00
    r.cpu.step(); r.cpu.step(); // ldi yh/yl
    CHECK(r.cpu.y() == 0x7BB2);
    CHECK(r.cpu.flagC());
    r.cpu.step(); // adr y
    CHECK(r.cpu.y() == 0x7BB2);  // Y + 0 == Y
    CHECK(r.cpu.flagC());        // carry must survive untouched

    // The internal carry must still propagate into RH even though it isn't
    // published: 0x00FF + 0x01 -> 0x0100.
    // rec ; ldi a,0x01 ; ldi xh,0x00 ; ldi xl,0xFF ; adr x
    Rig r2({0xF9, 0xB5, 0x01, 0x48, 0x00, 0x4A, 0xFF, 0xFD, 0xCA});
    r2.cpu.step(); // rec (reset carry)
    r2.cpu.step(); // ldi a,0x01
    r2.cpu.step(); r2.cpu.step(); // ldi xh/xl
    CHECK(r2.cpu.x() == 0x00FF);
    CHECK(!r2.cpu.flagC());
    r2.cpu.step(); // adr x
    CHECK(r2.cpu.x() == 0x0100); // high byte incremented from the internal carry
    CHECK(!r2.cpu.flagC());      // ...but the status register still says carry clear
}

void test_sjp_rtn_stack_roundtrip() {
    // At 0x8000: sjp 0x9000 ; (return lands here) ldi a,0x42
    // At 0x9000: rtn
    Rig r({0xBE, 0x90, 0x00, 0xB5, 0x42});
    r.bus.mem[0x9000] = 0x9A; // rtn
    uint16_t spBefore = r.cpu.sp();
    r.cpu.step(); // sjp
    CHECK(r.cpu.pc() == 0x9000);
    CHECK(r.cpu.sp() == uint16_t(spBefore - 2));
    r.cpu.step(); // rtn
    CHECK(r.cpu.pc() == 0x8003);
    CHECK(r.cpu.sp() == spBefore);
    r.cpu.step(); // ldi a,0x42
    CHECK(r.cpu.a() == 0x42);
}

void test_psh_pop_pair_roundtrip() {
    // ldi xh,0x12 ; ldi xl,0x34 ; psh x ; ldi xh,0 ; ldi xl,0 ; pop x
    Rig r({0x48, 0x12, 0x4A, 0x34, 0xFD, 0x88, 0x48, 0x00, 0x4A, 0x00, 0xFD, 0x0A});
    r.cpu.step(); r.cpu.step();
    CHECK(r.cpu.x() == 0x1234);
    r.cpu.step(); // psh x
    r.cpu.step(); r.cpu.step();
    CHECK(r.cpu.x() == 0x0000);
    r.cpu.step(); // pop x
    CHECK(r.cpu.x() == 0x1234);
}

void test_hlt_and_interrupt_wake() {
    // Vector target (0x9000) is separate from the code right after HLT,
    // so the IE-off (stays halted) and IE-on (vectors away) scenarios
    // below are clearly distinguishable by PC/A.
    Rig r({0xFD, 0xB1, 0xB5, 0x99}); // hlt (0x8000-01) ; ldi a,0x99 (0x8002-03)
    r.bus.mem[0xFFFA] = 0x90;
    r.bus.mem[0xFFFB] = 0x00; // interrupt vector -> 0x9000
    int c = r.cpu.step(); // hlt
    CHECK(c == 9);
    CHECK(r.cpu.halted());
    c = r.cpu.step(); // still halted, no interrupt yet
    CHECK(c == 0);
    CHECK(r.cpu.halted());

    // IE is off: requestMaskableInterrupt() must NOT wake HLT at all for
    // this interrupt class (timer/general maskable); only an IE-gated
    // dispatch clears the halted state (an NMI-class line would be
    // different, but this core doesn't model one -- see the .cpp comment).
    // The CPU must stay halted, ticking the timer but making no other
    // progress, until IE is actually set.
    CHECK(!r.cpu.flagIE());
    r.cpu.requestMaskableInterrupt();
    c = r.cpu.step(); // still halted -- the request doesn't wake it while IE is off
    CHECK(r.cpu.halted());
    CHECK(c == 0);
    CHECK(r.cpu.pc() == 0x8002); // unmoved -- never resumed, never vectored
    CHECK(r.cpu.a() != 0x99);    // "ldi a,0x99" never ran

    // Now with IE on, a fresh interrupt request must actually service.
    Rig r2({0xFD, 0xB1, 0xB5, 0x99});
    r2.bus.mem[0xFFFA] = 0x90;
    r2.bus.mem[0xFFFB] = 0x00;
    r2.cpu.step(); // hlt
    r2.cpu.setStatusReg(uint8_t(r2.cpu.statusReg() | 0x02)); // IE=1
    r2.cpu.requestMaskableInterrupt();
    c = r2.cpu.step(); // services the interrupt: pushes state, jumps to vector
    CHECK(!r2.cpu.halted());
    CHECK(r2.cpu.pc() == 0x9000);
    CHECK(r2.cpu.a() != 0x99); // vectored away -- "ldi a,0x99" did NOT execute
}

// wakeFromHalt() is the unconditional counterpart to
// requestMaskableInterrupt() -- the PC-1600's ON key needs it because a
// real power-down HALT is reached with IE clear (a headless real-ROM
// repro of the reported GUI freeze found the LH5803 parked exactly that
// way), so the ordinary IE-gated wake above can never reach it.
void test_wake_from_halt_ignores_ie() {
    Rig r({0xFD, 0xB1, 0xB5, 0x99}); // hlt (0x8000-01) ; ldi a,0x99 (0x8002-03)
    r.cpu.step(); // hlt
    CHECK(r.cpu.halted());
    CHECK(!r.cpu.flagIE());

    r.cpu.wakeFromHalt();
    CHECK(!r.cpu.halted());

    // No vector jump -- normal fetch/execute resumes right after the HLT,
    // same as an interrupt that ends HLT without being serviced.
    int c = r.cpu.step(); // ldi a,0x99
    CHECK(c > 0);
    CHECK(r.cpu.pc() == 0x8004);
    CHECK(r.cpu.a() == 0x99);
}

// OFF (0xFD 0x4C) is a genuine power-down, not a no-op and not a HLT: real
// hardware physically cuts the CPU clock, so nothing -- not even a pending
// or newly-arriving interrupt, and not the internal timer that would
// otherwise end an ordinary HLT -- can run again until powerOn() (the ON
// key's BFI pin) restores it with a real reset. This is what lets the
// ROM's own OFF-key/AUTO-POWER-OFF handler actually halt execution at
// FD 4C instead of falling through to whatever comes next.
void test_off_instruction_powers_down_until_power_on() {
    // off (0xFD 0x4C) ; ldi a,0x99 (would prove execution kept going)
    Rig r({0xFD, 0x4C, 0xB5, 0x99});
    r.cpu.setStatusReg(0x02); // IE=1, so a spurious wake isn't masked by IE
    int c = r.cpu.step(); // off
    CHECK(c > 0);
    CHECK(r.cpu.poweredOff());
    CHECK(!r.cpu.halted()); // a distinct state from HLT, not the same flag

    // Not even an interrupt can run the CPU while powered off.
    r.cpu.requestMaskableInterrupt();
    c = r.cpu.step();
    CHECK(c == 0);
    CHECK(r.cpu.poweredOff());
    CHECK(r.cpu.pc() == 0x8002); // unmoved -- "ldi a,0x99" never ran
    CHECK(r.cpu.a() != 0x99);

    // wakeFromHalt() (the HLT-only wake) must NOT clear a genuine power-off
    // -- they're deliberately separate states with separate wake paths
    // (PC1500Machine::setOnKeyPressed() picks between them).
    r.cpu.wakeFromHalt();
    CHECK(r.cpu.poweredOff());

    // powerOn() is a real reset -- lands at the reset vector (0x8000 here,
    // per Rig), not back where OFF left off.
    r.cpu.powerOn();
    CHECK(!r.cpu.poweredOff());
    CHECK(r.cpu.pc() == 0x8000);
    CHECK(r.cpu.statusReg() == 0); // IE cleared too, like any other reset
}

void test_rti_restores_flags_and_pc() {
    // sec ; sie ; ldi xh,0x90 ; ldi xl,0x00 -- prep an interrupt vector at 0x9000: rti
    Rig r({0xFB, 0xFD, 0x81, 0x48, 0x90, 0x4A, 0x00});
    r.bus.mem[0xFFFA] = 0x90;
    r.bus.mem[0xFFFB] = 0x00;
    r.bus.mem[0x9000] = 0x8A; // rti
    r.cpu.step(); // sec: C=1
    r.cpu.step(); // sie: IE=1
    CHECK(r.cpu.flagC() && r.cpu.flagIE());
    r.cpu.step(); r.cpu.step(); // ldi xh/xl -> P now at 0x8007
    uint16_t returnPC = r.cpu.pc();
    // Manually clear carry (no CPU step) to prove RTI restores the
    // pre-interrupt T value rather than leaving whatever T holds now.
    r.cpu.setFlagC(false);
    CHECK(!r.cpu.flagC());
    r.cpu.requestMaskableInterrupt();
    r.cpu.step(); // services interrupt: pushes T (C=0,IE=1) and P, jumps to 0x9000
    CHECK(r.cpu.pc() == 0x9000);
    CHECK(!r.cpu.flagIE()); // acceptance resets IE ...
    r.cpu.requestMaskableInterrupt(); // ... so a second request can't re-enter the handler
    // Simulate the handler mutating flags before returning -- RTI must undo this.
    r.cpu.setFlagC(true);
    r.cpu.step(); // rti runs; the second request is still held off
    CHECK(r.cpu.pc() == returnPC);
    CHECK(!r.cpu.flagC());  // restored to the pre-interrupt value, not the handler's
    CHECK(r.cpu.flagIE());  // RTI restores IE from the pushed T
    r.cpu.step(); // now the pending request is taken
    CHECK(r.cpu.pc() == 0x9000);
}

void test_trace_ring_and_drain() {
    Rig r({0xB5, 0x01, 0xB5, 0x02, 0xB5, 0x03}); // three ldi a,n instructions
    r.cpu.setTraceFlags(TRACE_PC | TRACE_REGS_LIGHT);
    r.cpu.step(); r.cpu.step(); r.cpu.step();
    CpuFrame frames[8];
    uint32_t lost = 0;
    uint32_t n = r.cpu.drainTraceEvents(frames, 8, &lost);
    CHECK(n == 3);
    CHECK(lost == 0);
    CHECK(frames[0].pc == 0x8000);
    CHECK(frames[0].opcode == 0x00B5);
    CHECK(frames[1].pc == 0x8002);
    CHECK(frames[2].pc == 0x8004);
    CHECK(frames[2].a == 0x03);
    // A second drain with nothing new produces nothing.
    n = r.cpu.drainTraceEvents(frames, 8, &lost);
    CHECK(n == 0);
}

void test_trace_ring_overflow_accounting() {
    // Ring size is 512; execute far more than that and confirm the drain
    // reports the correct lost count and starts from the oldest still-held frame.
    Rig r(std::vector<uint8_t>(2000, 0x38)); // 2000 NOPs
    r.cpu.setTraceFlags(TRACE_PC);
    const int kTotal = 1000;
    for (int i = 0; i < kTotal; i++) r.cpu.step();
    CpuFrame frames[600];
    uint32_t lost = 0;
    uint32_t n = r.cpu.drainTraceEvents(frames, 600, &lost);
    CHECK(lost == uint32_t(kTotal - 512));
    CHECK(n == 512);
    CHECK(frames[0].seqno == uint32_t(kTotal - 512));
    CHECK(frames[n - 1].seqno == uint32_t(kTotal - 1));
}

void test_trace_ring_peek_does_not_consume() {
    // peekTraceEvents() is for a debugger's frozen view: it must return a
    // stable snapshot of the most-recent frames without disturbing the
    // live drain cursor, so an ordinary (non-frozen) drainTraceEvents()
    // caller sees exactly what it would have without any peeking having
    // happened at all.
    Rig r({0xB5, 0x01, 0xB5, 0x02, 0xB5, 0x03}); // three ldi a,n instructions
    r.cpu.setTraceFlags(TRACE_PC | TRACE_REGS_LIGHT);
    r.cpu.step(); r.cpu.step();
    CpuFrame peeked[8];
    uint32_t n = r.cpu.peekTraceEvents(peeked, 8);
    CHECK(n == 2);
    CHECK(peeked[0].pc == 0x8000);
    CHECK(peeked[1].pc == 0x8002);
    CHECK(peeked[1].a == 0x02);
    // Peeking again returns the identical snapshot -- it's read-only.
    CpuFrame peekedAgain[8];
    n = r.cpu.peekTraceEvents(peekedAgain, 8);
    CHECK(n == 2);
    CHECK(peekedAgain[1].a == 0x02);
    // A real drain afterward still sees both frames -- peeking didn't
    // advance the drain cursor.
    CpuFrame drained[8];
    uint32_t lost = 0;
    n = r.cpu.drainTraceEvents(drained, 8, &lost);
    CHECK(n == 2);
    CHECK(lost == 0);
    CHECK(drained[1].a == 0x02);
    // A third instruction, then peek should reflect only the newest 1
    // when asked for max=1 (most-recent-first semantics), while a
    // larger max still returns everything available.
    r.cpu.step();
    CpuFrame onlyNewest[1];
    n = r.cpu.peekTraceEvents(onlyNewest, 1);
    CHECK(n == 1);
    CHECK(onlyNewest[0].pc == 0x8004);
    CHECK(onlyNewest[0].a == 0x03);
}

void test_breakpoint_halts_step() {
    Rig r({0xB5, 0x01, 0xB5, 0x02, 0xB5, 0x03});
    r.cpu.setBreakpointsEnabled(true);
    r.cpu.addBreakpoint(0x8002);
    int c = r.cpu.step(); // executes ldi a,0x01 (pc was 0x8000, not the breakpoint)
    CHECK(c != 0);
    CHECK(r.cpu.a() == 0x01);
    c = r.cpu.step(); // pc is now 0x8002 -- breakpoint fires before fetch
    CHECK(c == 0);
    CHECK(r.cpu.consumeBreakpointHit());
    CHECK(r.cpu.a() == 0x01); // instruction at the breakpoint did NOT execute
    CHECK(r.cpu.pc() == 0x8002);
    // Second call to consumeBreakpointHit returns false (consumed).
    CHECK(!r.cpu.consumeBreakpointHit());
    r.cpu.removeBreakpoint(0x8002);
    c = r.cpu.step(); // now runs past it
    CHECK(c != 0);
    CHECK(r.cpu.a() == 0x02);
}

void test_illegal_opcode_is_distinguishable_from_nop() {
    // 0x30 has no case in execute()'s switch and isn't in the vej range
    // (0xC0-0xFE even) -- an undocumented opcode. Execution still proceeds
    // (so a malformed stream doesn't wedge step()), but the event must be
    // reported distinctly from a real NOP.
    Rig r({0x30, 0x38}); // illegal ; nop
    CHECK(!r.cpu.consumeIllegalOpcodeHit()); // nothing hit yet
    int c = r.cpu.step(); // the illegal opcode
    CHECK(c != 0); // execution still advances
    CHECK(r.cpu.consumeIllegalOpcodeHit());
    CHECK(r.cpu.lastIllegalOpcodePC() == 0x8000);
    CHECK(r.cpu.lastIllegalOpcode() == 0x0030);
    CHECK(!r.cpu.consumeIllegalOpcodeHit()); // consumed: false on the next check

    r.cpu.step(); // nop -- a genuinely documented instruction
    CHECK(!r.cpu.consumeIllegalOpcodeHit()); // must not be reported as illegal
}

void test_flag_cost_gating_no_trace_by_default() {
    Rig r({0x38, 0x38, 0x38});
    CHECK(r.cpu.traceFlags() == TRACE_NONE);
    r.cpu.step(); r.cpu.step(); r.cpu.step();
    CpuFrame frames[8];
    uint32_t lost = 0;
    uint32_t n = r.cpu.drainTraceEvents(frames, 8, &lost);
    CHECK(n == 0); // nothing recorded when tracing is off
}

void test_memory_open_bus_and_ram_regions() {
    PC1500Memory mem;
    CHECK(mem.readME0(0x0000) == 0xFF); // Y0, no module: open bus
    CHECK(mem.readME0(0x8000) == 0xFF); // Y2, no module: open bus
    mem.writeME0(0x4000, 0xAB);
    CHECK(mem.readME0(0x4000) == 0xAB); // S0 user RAM
    mem.writeME0(0x57FF, 0xCD);
    CHECK(mem.readME0(0x57FF) == 0xCD); // last byte of S0+S1+S2
    CHECK(mem.readME0(0x5800) == 0xFF); // S3: open bus (no expansion)
    mem.writeME0(0x7800, 0x11);
    CHECK(mem.readME0(0x7800) == 0x11); // S7 system RAM
    mem.writeME0(0x7FFF, 0x22);
    CHECK(mem.readME0(0x7FFF) == 0x22);
}

void test_memory_display_ram_mirroring() {
    // Per PC-1500-Address-Decoding.md: the V2/V3 sub-decode only examines
    // AD8/DME0, so the 2KB S6 window (0x7000-0x77FF) collapses onto the
    // same 512 physical bytes 4 times.
    PC1500Memory mem;
    mem.writeME0(0x7600, 0x55);
    CHECK(mem.readME0(0x7000) == 0x55); // mirror at +0x000
    CHECK(mem.readME0(0x7200) == 0x55); // mirror at +0x200
    CHECK(mem.readME0(0x7400) == 0x55); // mirror at +0x400
    CHECK(mem.readME0(0x7600) == 0x55); // the "real" location
    mem.writeME0(0x71FF, 0x66);
    CHECK(mem.readME0(0x77FF) == 0x66);
}

// Per PC-1500-Address-Decoding.md §2.3/§4.2: the *plain* PC-1500's S7 block
// is backed by a TC5514 pair that only decodes A0-A9 (10 lines) across a
// 2KB (11-line) window, so &7800-&7BFF and &7C00-&7FFF alias the same 1024
// physical bytes, offset by &400 -- e.g. a write to &7C00 is electrically
// indistinguishable from a write to &7800. The PC-1500A replaces that pair
// with a full HM6116 (the same part already used for S0-S2) that decodes
// all 11 lines, giving &7800-&7FFF genuinely independent storage for the
// first time -- which is exactly why &7C01-&7FFF becomes the PC-1500A-only
// "machine language area" (§4.1).
//
void test_memory_pc1500a_system_ram_has_no_aliasing() {
    PC1500Memory mem(PC1500Variant::PC1500A);
    mem.writeME0(0x7800, 0xAA);
    mem.writeME0(0x7C00, 0xBB);
    CHECK(mem.readME0(0x7800) == 0xAA); // untouched by the &7C00 write
    CHECK(mem.readME0(0x7C00) == 0xBB); // independent cell, not an alias of &7800

    mem.writeME0(0x784F, 0x11); // end of the LH5801's CPU_STACK reserve
    mem.writeME0(0x7C4F, 0x22); // would alias &784F on a plain PC-1500; must not here
    CHECK(mem.readME0(0x784F) == 0x11);
    CHECK(mem.readME0(0x7C4F) == 0x22);
}

// Model-dependent counterpart: the plain PC-1500's S7 block is only
// backed by a TC5514 pair decoding A0-A9 (10 lines) across the 2KB
// (11-line) window, so &7C00-&7FFF must alias &7800-&7BFF at a fixed
// &400 offset.
void test_memory_pc1500_system_ram_has_aliasing() {
    PC1500Memory mem(PC1500Variant::PC1500);
    mem.writeME0(0x7800, 0xAA);
    CHECK(mem.readME0(0x7C00) == 0xAA); // &7C00 aliases &7800

    mem.writeME0(0x7C00, 0xBB);
    CHECK(mem.readME0(0x7800) == 0xBB); // and the alias is symmetric

    mem.writeME0(0x784F, 0x11);
    CHECK(mem.readME0(0x7C4F) == 0x11); // holds across the block, not just its first byte

    mem.writeME0(0x7BFF, 0x22);
    CHECK(mem.readME0(0x7FFF) == 0x22); // ...and its last byte
}

// Model-dependent counterpart to test_memory_open_bus_and_ram_regions:
// the plain PC-1500 only has 2KB of built-in user RAM (S0); S1/S2
// (&4800-&57FF), populated as built-in RAM on the PC-1500A, are open bus
// on the plain PC-1500 (module slot, no module attached).
void test_memory_pc1500_narrower_user_ram() {
    PC1500Memory mem(PC1500Variant::PC1500);
    mem.writeME0(0x4000, 0xAB);
    CHECK(mem.readME0(0x4000) == 0xAB); // S0: still built-in RAM
    mem.writeME0(0x47FF, 0xCD);
    CHECK(mem.readME0(0x47FF) == 0xCD); // last byte of S0

    CHECK(mem.readME0(0x4800) == 0xFF); // S1: open bus on the plain PC-1500
    mem.writeME0(0x4800, 0x99);
    CHECK(mem.readME0(0x4800) == 0xFF); // write silently discarded, matching open-bus semantics
    CHECK(mem.readME0(0x57FF) == 0xFF); // S2: also open bus
}

void test_rom_load_and_write_ignored() {
    PC1500Memory mem;
    std::vector<uint8_t> rom(16384, 0);
    rom[0] = 0xAA;
    rom[16383] = 0xBB;
    CHECK(mem.loadROM(rom.data(), rom.size()));
    CHECK(mem.readME0(0xC000) == 0xAA);
    CHECK(mem.readME0(0xFFFF) == 0xBB);
    mem.writeME0(0xC000, 0x99); // ROM is read-only
    CHECK(mem.readME0(0xC000) == 0xAA);

    std::vector<uint8_t> wrongSize(100, 0);
    CHECK(!mem.loadROM(wrongSize.data(), wrongSize.size()));
}

void test_keyboard_scan_active_low_matrix() {
    // Digit1 sits at IN0/PA2 in the real keyboard matrix (see
    // PC1500Keyboard.cpp). Strobing column 2 (bit2=0) with it pressed must
    // clear bit0 (row IN0) in the read-back byte; strobing any other single
    // column must leave the result untouched; strobing nothing (0xFF) must
    // also leave it untouched.
    PC1500Keyboard kb;
    kb.setKeyState(PC1500Keyboard::Key::Digit1, true);
    CHECK(kb.scan(0xFB) == 0xFE);  // column 2 strobed (~(1<<2) & 0xFF)
    CHECK(kb.scan(0xFE) == 0xFF);  // column 0 strobed -- Digit1 isn't there
    CHECK(kb.scan(0xFF) == 0xFF);  // no column strobed
    kb.setKeyState(PC1500Keyboard::Key::Digit1, false);
    CHECK(kb.scan(0xFB) == 0xFF);  // released
}

void test_keyboard_name_lookup() {
    CHECK(PC1500Keyboard::keyFromName("1") == PC1500Keyboard::Key::Digit1);
    CHECK(PC1500Keyboard::keyFromName("a") == PC1500Keyboard::Key::A);
    CHECK(PC1500Keyboard::keyFromName("Z") == PC1500Keyboard::Key::Z);
    CHECK(PC1500Keyboard::keyFromName("+") == PC1500Keyboard::Key::Plus);
    CHECK(PC1500Keyboard::keyFromName("cl") == PC1500Keyboard::Key::Cl);
    CHECK(PC1500Keyboard::keyFromName("enter") == PC1500Keyboard::Key::Ent);
    CHECK(PC1500Keyboard::keyFromName("ent") == PC1500Keyboard::Key::Ent);
    CHECK(PC1500Keyboard::keyFromName("f6") == PC1500Keyboard::Key::F6);
    CHECK(PC1500Keyboard::keyFromName("on") == PC1500Keyboard::Key::Unknown);   // deliberately not in the matrix
    CHECK(PC1500Keyboard::keyFromName("nonsense") == PC1500Keyboard::Key::Unknown);
}

void test_io_chip_keyboard_wiring_through_memory() {
    // Exercises the real ROM's own protocol (confirmed against real
    // hardware, see PC1500Memory.hpp's top comment): DDA at register 0xC,
    // OPA (column strobe) at register 0xE, both ME1-mapped and mirrored
    // across any address with bits 12-13 set.
    PC1500Memory mem;
    mem.writeME1(0xF00C, 0xFF); // DDA: PA all-output
    CHECK(mem.readME1(0xF00C) == 0xFF);
    mem.writeME1(0xF00E, 0xFB); // OPA: strobe column 2
    CHECK(mem.readME1(0xF00E) == 0xFB);

    mem.keyboard().setKeyState(PC1500Keyboard::Key::Digit1, true); // IN0/PA2
    CHECK(mem.readInputPort() == 0xFE);

    // Confirmed mirror: bits 14-15 don't matter, only bits 12-13.
    CHECK(mem.readME1(0xB00C) == 0xFF);
    mem.writeME1(0xB00E, 0xFF); // re-strobe nothing via the mirror address
    CHECK(mem.readInputPort() == 0xFF);

    // An ME1 address outside the I/O-chip's decode window (bits 12-13 not
    // both set) still falls back to mirroring ME0 -- the Phase 1 default.
    mem.writeME1(0x4100, 0x77);
    CHECK(mem.readME0(0x4100) == 0x77);
}

void test_rtc_tp_rate_and_gating() {
    // TP produces no edges at all until the ROM has issued at least one
    // rate-select command. Without this gate, an always-on TP from
    // power-on would spuriously set IF bit 1 during the boot prompt, well
    // before WAIT/BEEP ever configure it. See Upd1990ac::advance()'s own
    // comment.
    PC1500Memory mem;
    mem.advanceRtc(10'000'000); // ~7.7s at 1.3MHz -- plenty of time for a bug to show
    CHECK((mem.readME1(0xF00B) & 0x02) == 0);
    CHECK((mem.readME1(0xF00F) & 0x20) == 0);

    // Configure TP=64Hz: C2=1,C1=0,C0=0 -> value bit5=1(C2), bit3=0(C0),
    // bit4=0(C1); STB (bit1) must transition low->high to latch (a plain
    // single write with STB already set does nothing -- matches the real
    // command protocol's edge-triggered latch).
    mem.writeME1(0xF008, 0x20);
    mem.writeME1(0xF008, uint8_t(0x20 | 0x02));

    // 100,000 steps * 100 cycles = 10,000,000 cycles ~= 7.69s at 1.3MHz,
    // which should show ~492 ticks (64Hz) on IF bit 1, each cleared the
    // way the real ROM's own "ani #(0xF00B),0xFD" does (LC4C6/LC4ED) --
    // confirms both the rate and that an explicit write can still clear
    // the bit.
    int ticks = 0;
    for (int i = 0; i < 100'000; i++) {
        mem.advanceRtc(100);
        uint8_t ifVal = mem.readME1(0xF00B);
        if (ifVal & 0x02) {
            ticks++;
            mem.writeME1(0xF00B, uint8_t(ifVal & ~0x02));
        }
    }
    CHECK(ticks >= 480 && ticks <= 505); // ~492 expected, generous margin
}

void test_rtc_tp_phase_free_runs_across_configures() {
    // TP comes off the chip's free-running divider: a rate select doesn't
    // restart its phase, so rising edges stay on the 1/64 s grid however
    // the configures fall. (A real unit's BEEP repeat periods measure as
    // whole 64ths of a second.) At 1.3 MHz a 64 Hz period is 20312.5
    // cycles and rising edges fall at odd half-periods: 10156.25 + k*20312.5.
    PC1500Memory mem;
    uint64_t t = 0;
    auto advance = [&](uint32_t c) { mem.advanceRtc(c); t += c; };
    auto nextRisingEdge = [&]() {
        for (;;) {
            advance(10);
            uint8_t ifVal = mem.readME1(0xF00B);
            if (ifVal & 0x02) { mem.writeME1(0xF00B, uint8_t(ifVal & ~0x02)); return t; }
        }
    };
    auto configure64 = [&]() {
        mem.writeME1(0xF008, 0x20);
        mem.writeME1(0xF008, uint8_t(0x20 | 0x02));
    };
    auto offGrid = [](uint64_t cyc) {
        double k = (double(cyc) - 10156.25) / 20312.5;
        return std::abs(k - std::round(k)) * 20312.5; // cycles off the grid
    };
    advance(3333);
    configure64();
    CHECK(offGrid(nextRisingEdge()) <= 10);
    for (uint32_t skew : {777u, 5000u, 12345u}) {
        mem.writeME1(0xF008, 0x00); // Register Hold: TP off (WAIT/BEEP cleanup)
        mem.writeME1(0xF008, 0x02);
        advance(skew);
        configure64();
        CHECK(offGrid(nextRisingEdge()) <= 10);
    }
}

void test_rtc_if_does_not_clear_on_read() {
    // IF's TP flag (bit 1) must not clear on an ordinary read of the
    // register: the ROM's own MI interrupt handler (LE171) reads IF to
    // test a *different* bit for unrelated dispatch purposes on every
    // single BREAK-triggered interrupt, so a read that cleared bit 1 as a
    // side effect would silently eat the flag before the interpreter's
    // own statement-boundary break-check ever saw it.
    PC1500Memory mem;
    mem.writeME1(0xF008, 0x20);
    mem.writeME1(0xF008, uint8_t(0x20 | 0x02));
    mem.advanceRtc(20000); // one full 64Hz period, ~15625 cycles -- guarantees an edge
    CHECK((mem.readME1(0xF00B) & 0x02) != 0);
    // A second read (simulating the MI handler's own unrelated bit-0 test)
    // must NOT have cleared bit 1.
    CHECK((mem.readME1(0xF00B) & 0x02) != 0);
    // Only an explicit write clears it.
    mem.writeME1(0xF00B, 0x00);
    CHECK((mem.readME1(0xF00B) & 0x02) == 0);
}

void test_rtc_opb_and_if_never_disagree_within_one_poll() {
    // A real regression, reproduced live: pressing WAIT-driven PRINT
    // reported "BREAK AT <line>" within the first tick or two, every
    // time. Root cause -- IF's read (via the ROM's own E451 helper) was
    // firing independently of OPB's immediately-preceding read within the
    // *same* WAIT poll iteration (E89C: check OPB, ~15 cycles later check
    // IF), so it could see a "new" edge OPB's own check a few cycles
    // earlier hadn't -- and since IF is deliberately sticky (see
    // test_rtc_if_does_not_clear_on_read), any such disagreement, even
    // once, got misread as BREAK on every later iteration of the *same*
    // poll loop, not just the one where it happened. Simulates WAIT's own
    // poll cadence (OPB, then ~15 cycles later IF) across many simulated
    // ticks and confirms they always agree -- the exact invariant
    // Upd1990ac's debounce (see its own class doc comment) exists to
    // guarantee.
    PC1500Memory mem;
    mem.writeME1(0xF008, 0x20);
    mem.writeME1(0xF008, uint8_t(0x20 | 0x02)); // TP=64Hz

    int disagreements = 0;
    for (int iter = 0; iter < 2000; iter++) {
        mem.advanceRtc(50); // a handful of cycles between poll iterations
        bool opbHigh = (mem.readME1(0xF00F) & 0x20) != 0;
        mem.advanceRtc(15); // the real OPB-to-IF timing gap
        bool ifSet = (mem.readME1(0xF00B) & 0x02) != 0;
        if (ifSet) mem.writeME1(0xF00B, uint8_t(mem.readME1(0xF00B) & ~0x02));
        // IF must never show set while OPB's own immediately-preceding
        // read did NOT show high -- that combination is exactly what the
        // ROM's E89C branch structure treats as BREAK.
        if (ifSet && !opbHigh) disagreements++;
    }
    CHECK(disagreements == 0);
}

// ── uPD1990AC 40-bit BCD calendar (BASIC TIME / TIME=) ──────────────────
//
// Drives the exact port protocol the ROM's WRITE_2_CLOCK ($E52B) /
// TIMER_MODE ($E573) routines use, through PC1500Memory's I/O chip:
//   OPC (F008) bit0 = DATA IN, bit1 = STB, bit2 = CLK, bits3-5 = C0/C1/C2
//   OPB (F00F) bit6 (0x40) = DATA OUT
// C-mode select values (C0/C1/C2 in bits 3-5, so value = modeBits << 3):
//   0 Register Hold, 1 Register Shift, 2 Time Set, 3 Time Read.

namespace {
constexpr uint16_t kOpc = 0xF008;
constexpr uint16_t kOpb = 0xF00F;
constexpr uint8_t  kRegShift = 0x08; // C0=1 held on OPC during CLK pulses

// Latch a C-mode: present it with STB low, pulse STB high (the edge that
// latches), then STB low again -- the ROM's own ADI $02 / ADI $FE dance.
void rtcSetMode(PC1500Memory& mem, uint8_t modeBits) {
    uint8_t base = uint8_t(modeBits << 3);
    mem.writeME1(kOpc, base);
    mem.writeME1(kOpc, uint8_t(base | 0x02));
    mem.writeME1(kOpc, base);
}

// Time Read (snapshot the live clock), then Register Shift and clock 40
// bits out of DATA OUT, LSB first -- reassembled into the same 40-bit
// value liveTimeAsBcd40() produced.
uint64_t rtcReadCalendar40(PC1500Memory& mem) {
    rtcSetMode(mem, 3); // Time Read -> snapshot
    rtcSetMode(mem, 1); // Register Shift
    uint64_t v = 0;
    for (int i = 0; i < 40; i++) {
        if (mem.readME1(kOpb) & 0x40) v |= (uint64_t(1) << i); // DATA OUT read *before* the clock, as the ROM does
        mem.writeME1(kOpc, kRegShift);
        mem.writeME1(kOpc, uint8_t(kRegShift | 0x04)); // CLK rising -> shift right
        mem.writeME1(kOpc, kRegShift);
    }
    return v;
}

// Register Shift 40 bits in from DATA IN (LSB first), then Time Set ->
// Register Hold, which commits the shifted-in value to the live clock.
void rtcWriteCalendar40(PC1500Memory& mem, uint64_t value) {
    rtcSetMode(mem, 1); // Register Shift
    for (int i = 0; i < 40; i++) {
        uint8_t d = uint8_t(kRegShift | ((value >> i) & 1));
        mem.writeME1(kOpc, d);
        mem.writeME1(kOpc, uint8_t(d | 0x04)); // CLK rising -> shift, DATA IN -> bit 39
        mem.writeME1(kOpc, d);
    }
    rtcSetMode(mem, 2); // Time Set
    rtcSetMode(mem, 0); // Register Hold -> commit
}

int rtcNibble(uint64_t reg, int shift) { return int((reg >> shift) & 0x0F); }
} // namespace

void test_rtc_calendar_seed_and_read() {
    // Seed a known instant and walk it back out via the ROM's exact
    // read protocol: the 40-bit register must carry it field-for-field.
    PC1500Memory mem;
    mem.seedClock(2026, 9, 2, 14, 30, 15, /*dow=*/3); // Wed 2 Sep 2026 14:30:15
    uint64_t r = rtcReadCalendar40(mem);
    CHECK(rtcNibble(r, 0) == 5 && rtcNibble(r, 4) == 1);   // seconds 15
    CHECK(rtcNibble(r, 8) == 0 && rtcNibble(r, 12) == 3);  // minutes 30
    CHECK(rtcNibble(r, 16) == 4 && rtcNibble(r, 20) == 1); // hours 14
    CHECK(rtcNibble(r, 24) == 2 && rtcNibble(r, 28) == 0); // day 02
    CHECK(rtcNibble(r, 32) == 3);                          // day-of-week (Wed)
    CHECK(rtcNibble(r, 36) == 9);                          // month (plain, not BCD)
}

// `- syncclock:` re-seeds the RTC from the host: after a 10-minute
// emulated `- wait:` (the clock would otherwise be ~10 min ahead of the
// boot seed), the calendar reads back the host's current time. ROM-gated.
void test_preset_syncclock_reseeds_rtc() {
    PresetFile preset;
    std::string err;
    CHECK(parsePresetString("model: PC-1500A\nkeys:\n  - wait: 600\n  - syncclock:\n",
                            "/tmp/lh5801_tests_syncclock.pc1500a", &preset, &err));
    PC1500Machine machine(PC1500Variant::PC1500A);
    auto hostOnBoot = [&machine] {
        machine.seedClock(2000, 1, 1, 0, 0, 0); // a clearly wrong clock to start from
    };
    const PresetLoadResult res = applyPC1500Preset(machine, preset, {}, ".", ".", hostOnBoot, {"roms"});
    if (!res.ok) {
        std::fprintf(stderr, "SKIP test_preset_syncclock_reseeds_rtc: %s\n", res.error.c_str());
        return;
    }
    const std::time_t now = std::time(nullptr);
    std::tm t{};
    localtime_r(&now, &t);
    const uint64_t r = rtcReadCalendar40(machine.memory());
    auto bcd = [&](int shift) { return rtcNibble(r, shift + 4) * 10 + rtcNibble(r, shift); };
    const int rtcMinutes = bcd(16) * 60 + bcd(8);
    const int hostMinutes = t.tm_hour * 60 + t.tm_min;
    CHECK(rtcNibble(r, 36) == t.tm_mon + 1);
    CHECK(bcd(24) == t.tm_mday || t.tm_hour == 0);          // tolerate a midnight rollover
    const int diff = (hostMinutes - rtcMinutes + 1440) % 1440;
    CHECK(diff <= 1);                                         // not 10 minutes ahead
}

void test_rtc_calendar_set_via_shift_and_commit() {
    // TIME= path: shift 40 bits in, Time Set -> Register Hold to commit,
    // then a Time-Read round-trip must return exactly what was written.
    PC1500Memory mem;
    mem.seedClock(2000, 1, 1, 0, 0, 0, 6);

    uint64_t want = 0;
    want |= uint64_t(0x00) << 0;   // seconds 00
    want |= uint64_t(0x45) << 8;   // minutes 45
    want |= uint64_t(0x09) << 16;  // hours 09
    want |= uint64_t(0x10) << 24;  // day 10
    want |= uint64_t(0x05) << 32;  // day-of-week
    want |= uint64_t(0x03) << 36;  // month 3

    rtcWriteCalendar40(mem, want);
    CHECK(rtcReadCalendar40(mem) == want);
}

void test_rtc_calendar_time_set_straight_to_time_read() {
    // Time Set -> Time Read with no Register Hold in between: the commit
    // on leaving Time Set must land before Time Read's snapshot, or the
    // read returns the old time and the TIME= value is lost.
    PC1500Memory mem;
    mem.seedClock(2000, 1, 1, 0, 0, 0, 6);
    const uint64_t want = (uint64_t(0x04) << 36) | (uint64_t(0x02) << 32) | (uint64_t(0x21) << 24) |
                          (uint64_t(0x17) << 16) | (uint64_t(0x08) << 8) | uint64_t(0x33);
    rtcSetMode(mem, 1); // Register Shift
    for (int i = 0; i < 40; i++) {
        uint8_t d = uint8_t(kRegShift | ((want >> i) & 1));
        mem.writeME1(kOpc, d);
        mem.writeME1(kOpc, uint8_t(d | 0x04));
        mem.writeME1(kOpc, d);
    }
    rtcSetMode(mem, 2); // Time Set
    CHECK(rtcReadCalendar40(mem) == want); // starts with Time Read
}

void test_rtc_calendar_advances_one_hz_with_bcd_and_month_carry() {
    // One tick per emulated second, with BCD carry rippling sec->min->
    // hour->day and a Jan(31)->Feb month rollover. 2026 is not a leap
    // year, so January has 31 days.
    PC1500Memory mem;
    mem.seedClock(2026, 1, 31, 23, 59, 58, 6);
    mem.advanceRtc(3u * 1'300'000u); // ~3 emulated seconds at 1.3MHz

    uint64_t r = rtcReadCalendar40(mem);
    CHECK(rtcNibble(r, 36) == 2);                          // month rolled Jan -> Feb
    CHECK(rtcNibble(r, 24) == 1 && rtcNibble(r, 28) == 0); // day 01
    CHECK(rtcNibble(r, 16) == 0 && rtcNibble(r, 20) == 0); // hour 00
    CHECK(rtcNibble(r, 8) == 0 && rtcNibble(r, 12) == 0);  // minute 00
    CHECK(rtcNibble(r, 0) == 1 && rtcNibble(r, 4) == 0);   // 58 -> 59 -> 00 -> 01
}

void test_rtc_seed_millisecond_aligns_next_tick() {
    // seedClock()'s millisecond preloads the 1 Hz accumulator: seeded at
    // .900, the next tick is 0.1 s away, not a full second.
    PC1500Memory mem;
    mem.seedClock(2026, 9, 23, 12, 0, 10, 3, 900);
    mem.advanceRtc(260'000u); // 0.2 s at 1.3MHz
    uint64_t r = rtcReadCalendar40(mem);
    CHECK(rtcNibble(r, 0) == 1 && rtcNibble(r, 4) == 1); // 10 -> 11

    PC1500Memory mem0;
    mem0.seedClock(2026, 9, 23, 12, 0, 10, 3, 0);
    mem0.advanceRtc(260'000u);
    r = rtcReadCalendar40(mem0);
    CHECK(rtcNibble(r, 0) == 0 && rtcNibble(r, 4) == 1); // still 10
}

void test_rtc_calendar_reset_is_deterministic() {
    // reset() must NOT leak host time into the Core -- headless tests and
    // the CLI depend on a fixed cold-start clock. The app re-seeds real
    // time via PC1500MachineWrapper -reset, not here.
    PC1500Memory a;
    PC1500Memory b;
    CHECK(rtcReadCalendar40(a) == rtcReadCalendar40(b));

    a.seedClock(2011, 7, 4, 12, 34, 56, 1);
    CHECK(rtcReadCalendar40(a) != rtcReadCalendar40(b));
    a.reset();
    CHECK(rtcReadCalendar40(a) == rtcReadCalendar40(b));
}

void test_machine_seedclock_derives_day_of_week() {
    // PC1500Machine::seedClock computes the dow nibble itself (Sakamoto),
    // so callers pass only the date. 2000-01-01 was a Saturday (6),
    // 2026-09-02 a Wednesday (3).
    PC1500Machine m1;
    m1.seedClock(2000, 1, 1, 0, 0, 0);
    CHECK(rtcNibble(rtcReadCalendar40(m1.memory()), 32) == 6);

    PC1500Machine m2;
    m2.seedClock(2026, 9, 2, 8, 0, 0);
    CHECK(rtcNibble(rtcReadCalendar40(m2.memory()), 32) == 3);
}

void test_on_key_press_sets_break_flag() {
    // Pressing ON while a program is running must set IF bit 1 (BREAK) for
    // the ROM's statement-boundary check (LC42A) and WAIT's poll loop
    // (0xE89C) to see, both of which test exactly this bit via the vectored
    // 0xA6 routine. The MI interrupt handler (LE171, vector 0xFFF8) only
    // tests/clears IF bit 0 (serial receive-done, unrelated), so nothing
    // else in the ROM sets bit 1 on an ON-key press. See
    // PC1500Memory::setOnKeyPressed()'s own doc comment for why a direct
    // write on the press transition is the right model.
    PC1500Machine machine;
    CHECK((machine.memory().readME1(0xF00B) & 0x02) == 0);
    machine.setOnKeyPressed(true);
    CHECK((machine.memory().readME1(0xF00B) & 0x02) != 0);
    machine.memory().writeME1(0xF00B, 0x00);

    // Holding it down must not re-set the flag on every call -- only the
    // press *transition* should (matches a real interrupt-request pulse,
    // not a level).
    machine.setOnKeyPressed(true);
    CHECK((machine.memory().readME1(0xF00B) & 0x02) == 0);

    machine.setOnKeyPressed(false);
    machine.setOnKeyPressed(true);
    CHECK((machine.memory().readME1(0xF00B) & 0x02) != 0);
}

// A key whose host release never arrived (the GUI lost track of it) must
// not outlive a reset -- otherwise the machine stays wedged on it.
void test_pc1500_reset_releases_held_keys() {
    PC1500Machine machine;
    machine.pressKey("*");
    CHECK(machine.memory().keyboard().scan(0x00) != 0xFF);
    machine.reset();
    CHECK(machine.memory().keyboard().scan(0x00) == 0xFF);
}

// User RAM powers up 0x00 (CMOS RAM after a power loss) but the 1.5K window
// &7600-&7BFF powers up 0xFF (measured on a real PC-1500); the reset button
// keeps RAM (a real RESET only resets the CPU/chips), Reset All clears it.
void test_pc1500_ram_powerup_reset_and_all_reset() {
    PC1500Machine machine;
    CHECK(machine.memory().peek(0x40C5) == 0x00);  // user RAM
    CHECK(machine.memory().peek(0x7800) == 0xFF);  // system RAM
    CHECK(machine.memory().peek(0x79FF) == 0xFF);  // LOCK register
    CHECK(machine.memory().peek(0x7600) == 0xFF);  // display RAM
    CHECK(machine.memory().peek(0x7BFF) == 0xFF);
    machine.memory().poke(0x40C5, 0x5A);
    machine.memory().poke(0x7800, 0xA5);
    machine.reset();
    CHECK(machine.memory().peek(0x40C5) == 0x5A);
    CHECK(machine.memory().peek(0x7800) == 0xA5);
    machine.allReset();
    CHECK(machine.memory().peek(0x40C5) == 0x00);
    CHECK(machine.memory().peek(0x7800) == 0xFF);
}

// AUTO POWER OFF / the OFF key park the CPU in a genuine HLT with the
// timer stopped (TM==0), matching real hardware cutting the clock --
// LH5801::tickTimer() is then a no-op (see its own doc comment), so the
// timer-driven interrupt that normally ends an idle HLT never fires again.
// PC1500Machine::setOnKeyPressed() must therefore also directly wake a
// halted CPU, not just latch the pollable IF bit 1 (see
// test_on_key_press_sets_break_flag), which a halted CPU that isn't
// polling anything would never see. Same shape as
// PC1600Machine::setOnKeyPressed()'s LH5803 wake
// (test_wake_from_halt_ignores_ie / PC1600's own machine tests).
void test_on_key_wakes_a_halted_cpu_with_timer_stopped() {
    PC1500Machine machine;
    machine.reset();
    // FD B1 = HLT, parked directly in RAM ($7800, System RAM) rather than
    // depending on ROM contents.
    machine.memory().poke(0x7800, 0xFD);
    machine.memory().poke(0x7801, 0xB1);
    machine.memory().poke(0x7802, 0xB5); // ldi a,0x42 right after the HLT
    machine.memory().poke(0x7803, 0x42);
    machine.cpu().setPC(0x7800);
    machine.cpu().setStatusReg(0x00); // IE=0, matching a real power-down HALT

    machine.step(); // hlt
    CHECK(machine.cpu().halted());
    CHECK(machine.cpu().statusReg() == 0);

    // Timer stopped (TM==0, the reset default) -- confirm step() alone
    // never wakes it, no matter how many times it's called.
    for (int i = 0; i < 1000; i++) machine.step();
    CHECK(machine.cpu().halted());

    machine.setOnKeyPressed(true);
    CHECK(!machine.cpu().halted());
    machine.step(); // ldi a,0x42 -- normal fetch resumes right after the HLT
    CHECK(machine.cpu().a() == 0x42);
}

// At the PC1500Machine level, waking a genuinely powered-off CPU (see
// test_off_instruction_powers_down_until_power_on()'s comment on the
// LH5801 side) must go through a real reset (powerOn()), not merely
// resume fetch/execute where a HLT left off (wakeFromHalt(), which
// test_on_key_wakes_a_halted_cpu_with_timer_stopped() covers instead).
void test_on_key_powers_a_powered_off_machine_back_on() {
    PC1500Machine machine;
    machine.reset();
    machine.memory().poke(0x7800, 0xFD);
    machine.memory().poke(0x7801, 0x4C); // off
    machine.cpu().setPC(0x7800);
    machine.cpu().setStatusReg(0x02); // IE=1 -- must still stay off

    machine.step(); // off
    CHECK(machine.cpu().poweredOff());

    for (int i = 0; i < 1000; i++) machine.step();
    CHECK(machine.cpu().poweredOff()); // never comes back on its own

    machine.setOnKeyPressed(true);
    CHECK(!machine.cpu().poweredOff());
    // A real reset, not a HLT-resume -- lands back at the ROM's reset
    // vector, not at 0x7802 (right after the OFF instruction).
    CHECK(machine.cpu().pc() != 0x7802);
}

void test_machine_presskey_releasekey_named_api() {
    PC1500Machine machine;
    machine.memory().writeME1(0xF00C, 0xFF); // DDA: PA all-output -- a PA bit only
                                              // actually strobes its column once DDA
                                              // has configured it as an output (see
                                              // PC1500Memory.hpp's m_dda doc comment)
    machine.memory().writeME1(0xF00E, 0xFB); // simulate firmware strobing column 2
    machine.pressKey("1");
    CHECK(machine.memory().readInputPort() == 0xFE);
    machine.releaseKey("1");
    CHECK(machine.memory().readInputPort() == 0xFF);

    // Unknown names (including "on" -- see PC1500Machine's doc comment)
    // are silently ignored, matching a real keyboard's behavior for a
    // matrix position that isn't wired to anything.
    machine.pressKey("on");
    machine.pressKey("not-a-real-key");
    CHECK(machine.memory().readInputPort() == 0xFF);
}

// PC1500Machine::advanceKeyQueue()'s cadence (PC1500BasicTyper.cpp's own
// tapKey() constants, duplicated there): kTapCycles = kGapCycles =
// kCyclesPerFrame(21666) * 4 = 86664.
namespace {
constexpr uint64_t kKeyQueueTapCycles = 86664;
constexpr uint64_t kKeyQueueGapCycles = 86664;
}

void test_machine_enqueuekey_paces_press_hold_release_gap() {
    PC1500Machine machine;
    machine.memory().writeME1(0xF00C, 0xFF); // DDA: PA all-output
    machine.memory().writeME1(0xF00E, 0xFB); // strobe column 2 -- Digit1 (IN0) and Digit4 (IN1) both live there

    CHECK(machine.memory().readInputPort() == 0xFF); // nothing queued yet

    machine.enqueueKey("1");
    machine.enqueueKey("4");

    // First runCycles() call dequeues+presses "1" immediately (Idle ->
    // Holding happens on the very first advanceKeyQueue() call).
    machine.runCycles(1);
    CHECK(machine.memory().readInputPort() == 0xFE); // IN0 (Digit1) held low

    // Past the hold window -> "1" released, now in its idle gap; "4"
    // hasn't started yet (still queued behind the gap).
    machine.runCycles(kKeyQueueTapCycles + 5000);
    CHECK(machine.memory().readInputPort() == 0xFF);

    // Gap elapsed -> "4" dequeued and pressed.
    machine.runCycles(kKeyQueueGapCycles + 5000);
    CHECK(machine.memory().readInputPort() == 0xFD); // IN1 (Digit4) held low

    // "4" released, its own gap running out; queue now empty.
    machine.runCycles(kKeyQueueTapCycles + kKeyQueueGapCycles + 5000);
    CHECK(machine.memory().readInputPort() == 0xFF);
}

void test_machine_enqueuekey_capacity_drops_newest() {
    PC1500Machine machine;
    machine.memory().writeME1(0xF00C, 0xFF);
    machine.memory().writeME1(0xF00E, 0xFB); // Digit1 = IN0/col2

    // Request 20 taps -- only kKeyQueueCapacity (16) are accepted; the
    // rest are silently dropped, matching real hardware's own behavior
    // under fast typing (see enqueueKey()'s doc comment), not counted as
    // a bug to fix here.
    for (int i = 0; i < 20; i++) machine.enqueueKey("1");

    int pressEvents = 0;
    bool wasPressed = false;
    uint64_t budget = 16ull * (kKeyQueueTapCycles + kKeyQueueGapCycles) + 200000ull;
    uint64_t consumed = 0;
    while (consumed < budget) {
        machine.runCycles(1000);
        consumed += 1000;
        bool isPressed = (machine.memory().readInputPort() == 0xFE);
        if (isPressed && !wasPressed) pressEvents++;
        wasPressed = isPressed;
    }
    CHECK(pressEvents == 16);
}

void test_machine_enqueuekey_ignores_unmapped_name() {
    PC1500Machine machine;
    machine.memory().writeME1(0xF00C, 0xFF);
    machine.memory().writeME1(0xF00E, 0xFB);

    // Matches pressKey()'s own convention: an unmapped name (including
    // "on") never reaches the matrix, and doesn't occupy a queue slot
    // that a real keystroke behind it would need.
    machine.enqueueKey("on");
    machine.enqueueKey("not-a-real-key");
    machine.enqueueKey("1");
    machine.runCycles(1);
    CHECK(machine.memory().readInputPort() == 0xFE); // "1" pressed immediately, not stuck behind the ignored names
}

void test_display_pixel_decode() {
    PC1500Memory mem;
    // col 0: block=0 (low nibble), half=0, addr=0x7600/0x7601.
    mem.poke(0x7600, 0x05); // nibble0 = 0b0101
    mem.poke(0x7601, 0x0A); // nibble1 = 0b1010 -> data8 = 0xA5 = 0b10100101
    PC1500Display disp(mem);
    CHECK(disp.pixel(0, 0) == true);  // bit0
    CHECK(disp.pixel(0, 1) == false); // bit1
    CHECK(disp.pixel(0, 2) == true);  // bit2
    CHECK(disp.pixel(0, 3) == false); // bit3
    CHECK(disp.pixel(0, 4) == false); // bit4
    CHECK(disp.pixel(0, 5) == true);  // bit5
    CHECK(disp.pixel(0, 6) == false); // bit6

    // col 78: block=1 (high nibble) of the *same* 0x7600/0x7601 byte pair --
    // both high nibbles are 0 here, so every row must read false.
    for (int row = 0; row < 7; row++) CHECK(disp.pixel(78, row) == false);

    // col 39: half=1, local=0 -> addr=0x7700/0x7701. PC1500Display is a
    // point-in-time snapshot (copied at construction, not a live view --
    // see its class doc comment), so pokes made after `disp` was built
    // require a fresh snapshot to be visible.
    mem.poke(0x7700, 0x03); // 0b0011
    mem.poke(0x7701, 0x0C); // 0b1100 -> data8 = 0xC3 = 0b11000011
    PC1500Display disp2(mem);
    CHECK(disp2.pixel(39, 0) == true);
    CHECK(disp2.pixel(39, 1) == true);
    CHECK(disp2.pixel(39, 2) == false);
    CHECK(disp2.pixel(39, 6) == true);

    // Out-of-range coordinates are false, not undefined behavior.
    CHECK(disp.pixel(-1, 0) == false);
    CHECK(disp.pixel(156, 0) == false);
    CHECK(disp.pixel(0, 7) == false);
}

void test_display_status_icons() {
    PC1500Memory mem;
    mem.poke(0x764E, 0xA5); // 0b10100101
    mem.poke(0x764F, 0x53); // 0b01010011
    PC1500Display disp(mem);
    CHECK(disp.busy() == true);
    CHECK(disp.shift() == false);
    CHECK(disp.japanese() == true);
    CHECK(disp.small() == false);
    CHECK(disp.romanIII() == false);
    CHECK(disp.romanII() == true);
    CHECK(disp.romanI() == false);
    CHECK(disp.def() == true);
    CHECK(disp.de() == true);
    CHECK(disp.g() == true);
    CHECK(disp.rad() == false);
    CHECK(disp.reserve() == true);
    CHECK(disp.pro() == false);
    CHECK(disp.run() == true);
}

void test_boot_smoke_real_rom() {
    PC1500Machine machine;
    bool loaded = machine.loadROMFile("roms/PC-1500_A04.ROM");
    if (!loaded) {
        std::fprintf(stderr, "SKIP test_boot_smoke_real_rom: roms/PC-1500_A04.ROM not found "
                              "relative to cwd (run tests from the repo root)\n");
        return;
    }
    machine.reset();
    CHECK(machine.cpu().pc() == 0xE000); // confirmed reset vector target, see LH5801.cpp comment

    // The ROM's documented input-buffer-clear fill loop (DEL_DIM_VAR_4,
    // $D0B0 per PC-1500-Address-Decoding.md §4.2) must be reached during a
    // real cold boot -- a source-independent correctness signal that
    // doesn't depend on decoding the LCD's pixel format.
    machine.setTraceFlags(TRACE_PC);
    bool sawKnownRomRoutine = false;
    for (int i = 0; i < 600000 && !sawKnownRomRoutine; i++) {
        if (machine.cpu().pc() == 0xD0B0) sawKnownRomRoutine = true;
        machine.step();
    }
    CHECK(sawKnownRomRoutine);
    machine.setTraceFlags(TRACE_NONE);

    // Cold boot must reach a genuine HLT at 0xE2AA, this ROM's own
    // "IDLE -- BASIC's stable idle/ready-prompt address", and it must get
    // there quickly (well under 1M cycles). Reaching idle depends on the
    // I/O-chip scratch registers actually holding writes instead of
    // returning a hardcoded constant, and on the timer advancing while the
    // CPU is halted so a timer-driven HLT wake can fire.
    uint64_t consumed = machine.runCycles(1'000'000);
    CHECK(consumed > 0);
    CHECK(machine.cpu().halted());
    CHECK(machine.cpu().pc() == 0xE2AA);

    // Shared by every key-press check below: tap one key, then confirm the
    // machine returns cleanly to that same idle address rather than
    // wedging or wandering off -- a meaningful, checkable round-trip
    // through the keyboard matrix -> LH5811 OPA/ITA wiring -> ROM key
    // dispatch -> HLT-idle path this phase built. Exact real-hardware
    // key-press timing isn't reproducible headlessly, so the hold/idle
    // cycle counts here are just generous, not calibrated.
    //
    // The settle budget is 8M cycles: KEYSCAN_NOWAIT's real per-key decode
    // (0xE42C) scans one column at a time by cycling DDA while OPA stays
    // fixed at 0 (see PC1500Memory.hpp's m_dda comment), so readInputPort()
    // must honor DDA rather than reporting every column as "strobed" on
    // the first iteration -- a real multi-column scan takes meaningfully
    // longer. 8M comfortably covers the slowest observed case (Ent's
    // check below, which settles by ~3M).
    auto tapAndExpectIdle = [&](const char* key) {
        machine.pressKey(key);
        machine.runCycles(200000);
        machine.releaseKey(key);
        uint64_t c = machine.runCycles(8'000'000);
        CHECK(c > 0);
        CHECK(machine.cpu().halted());
        CHECK(machine.cpu().pc() == 0xE2AA);
    };

    tapAndExpectIdle("cl");

    // Pressing Ent on a bare, empty input buffer at boot must return to
    // idle within budget: this exercises aluSub()'s carryIn handling and
    // CPA/CPI/CIN's carry-in convention, both load-bearing for the ROM's
    // Ent-key dispatch. Checked as its own case, distinct from the
    // "NEW 0" sequence below, since the empty-buffer path is a separate
    // code path from Ent on a non-empty buffer.
    tapAndExpectIdle("enter");

    // Typing "NEW 0" then Ent must also return to idle from a non-empty
    // input buffer, not just a bare Ent press.
    for (const char* k : {"n", "e", "w", "space", "0"}) {
        machine.pressKey(k);
        machine.runCycles(150000);
        machine.releaseKey(k);
        machine.runCycles(150000);
    }
    tapAndExpectIdle("enter");
}

} // namespace

// Defined in connector_tests.cpp -- kept as a separate translation
// unit/file since it exercises a distinct layer (Core/Connector/), but
// folded into this file's single test-runner executable/exit code per
// tools/run_tests.sh's existing single-binary convention.
int run_connector_tests();
// Defined in ce155_tests.cpp / ce1638_tests.cpp / ce163f_tests.cpp (the
// bundled CE-155, CE-1638 and CE-163F module definitions) -- same
// single-binary convention.
int run_ce155_tests();
int run_ce1638_tests();
int run_ce502b_tests();
int run_ce163f_tests();
// Defined in memory_card_tests.cpp (universal software-defined module) --
// same single-binary convention.
int run_memory_card_tests();
// Defined in battery_card_instance_tests.cpp (addressed-hex writer +
// battery-card instance splice logic) -- same single-binary convention.
int run_battery_card_instance_tests();
// Defined in preset_tests.cpp (.pc1500 preset parser) -- same
// single-binary convention.
int run_preset_tests();
// Defined in basictyper_tests.cpp (PC1500BasicTyper's scripted keystroke
// typing) -- same single-binary convention.
int run_basictyper_tests();
// Defined in presetloader_trace_tests.cpp (the preset loader's `- trace:`
// step, end-to-end against a real ROM) -- same single-binary convention.
int run_presetloader_trace_tests();
// Defined in pc1600_basictyper_tests.cpp (PC1600BasicTyper's scripted
// keystroke typing) -- same single-binary convention.
int run_pc1600_basictyper_tests();
// Defined in piezo_sampler_tests.cpp (buzzer audio) -- same single-binary
// convention.
int run_piezo_sampler_tests();
// Defined in pc1600_bank_tests.cpp (PC1600Bank/PC1600Memory bank-switching
// truth tables) -- same single-binary convention.
int run_pc1600_bank_tests();
// Defined in sc7852_tests.cpp (SC7852 Z-80-compatible core) -- same
// single-binary convention.
int run_sc7852_tests();
// Defined in tc8576f_tests.cpp (TC8576F UART model + sub-CPU parallel
// handshake timing) -- same single-binary convention.
int run_tc8576f_tests();
// Defined in lh5803_tests.cpp (LH5803 core + standalone LH5803Memory) --
// same single-binary convention.
int run_lh5803_tests();
// Defined in pc1600_machine_tests.cpp (dual-CPU facade, bus arbiter,
// forced handoff) -- same single-binary convention.
int run_pc1600_machine_tests();
// Defined in pc1600_phase54_tests.cpp (remaining ROM images, inert
// unmapped images) -- same single-binary convention.
int run_pc1600_phase54_tests();
// Defined in pc1600_keyboard_display_tests.cpp (PC1600Keyboard/
// PC1600Display and their PC1600Memory I/O wiring) -- same single-binary
// convention.
int run_pc1600_keyboard_display_tests();
// Defined in pc1600_slot_ram_tests.cpp (PC1600Memory Slot 1/2 RAM
// attachment) -- same single-binary convention.
int run_pc1600_slot_ram_tests();
// Defined in pc1600_slot_module_tests.cpp -- real module definitions
// plugged into a PC-1600 memory-slot connector, end to end.
int run_pc1600_slot_module_tests();
// Defined in pc1600_preset_tests.cpp -- the `model: PC-1600` preset
// parser extension + PC1600PresetLoader.
int run_pc1600_preset_tests();
// Defined in ce1600p_tests.cpp -- AlpsPlotterMechanism/CE1600PCard, the
// CE-1600P plotter's motor-phase-to-pen-stroke simulation.
int run_ce1600p_tests();
// Defined in ce1600f_tests.cpp -- CE1600FCard, the CE-1600F floppy's
// register-level protocol (confirmed via ROM disassembly).
int run_ce1600f_tests();
// Defined in ce150_tests.cpp -- Ce150Card (LH5810 + ROM window decode) and
// its PC1500Machine / 60-pin SystemBus integration.
int run_ce150_tests();
// Defined in pc1600_ce150_tests.cpp -- the CE-150 attached to the PC-1600's
// LH5803 side (Phase 2).
int run_pc1600_ce150_tests();
// Defined in ce158_tests.cpp -- Ce158Card (ROM window, LH5811, CDP1854 UART,
// Centronics) and the CE-158 driven by BASIC on a PC1500Machine.
int run_ce158_tests();
// Defined in basic_binary_image_tests.cpp -- the SharpDataExchange
// tokenized-BASIC transfer-file parser (Core/Basic/BasicBinaryImage).
int run_basic_binary_image_tests();
// Defined in basic_fastloader_tests.cpp -- PC1500BasicLoader (poke +
// BASPRG_END fix), checked byte-for-byte against the keystroke typer.
int run_basic_fastloader_tests();
// Defined in pc1600_basicloader_tests.cpp -- PC1600BasicLoader, same
// byte-for-byte check against the PC-1600 keystroke typer.
int run_pc1600_basicloader_tests();
// Defined in basic_program_source_tests.cpp -- readBasicProgramSource:
// .bas-vs-tokenized dispatch + libsharpdx tokenize-on-load.
int run_basic_program_source_tests();
// Defined in pc1600_program_placement_tests.cpp -- the scattered-bank
// segment-list + placement logic for the fast BASIC loader.
int run_pc1600_program_placement_tests();
// Defined in pc1600_machine_image_tests.cpp -- the 16-byte PC-1600
// machine-language transfer-header parser (Core/PC1600/PC1600MachineImage).
int run_pc1600_machine_image_tests();
// Defined in key_paste_tests.cpp -- the GUI's clipboard-paste feeder.
int run_key_paste_tests();
// Defined in lcd_screenshot_tests.cpp -- LCD PNG render + `screenshot:` step.
int run_lcd_screenshot_tests();
// Defined in machine_code_file_tests.cpp -- "Load Machine Code…": header
// recognition, load plan, NEW/CALL advice, and the two writers.
int run_machine_code_file_tests();
int run_disasm_tests();
int run_debug_target_tests();
int run_listing_tests();
int run_run_control_tests();

int main() {
    test_reset_vector();
    test_ldi_a_and_zero_flag();
    test_adc_carry_and_overflow();
    test_adc_signed_overflow_v_flag();
    test_sbc_no_borrow_convention();
    test_cpa_flags_no_writeback();
    test_lop_iterates_n_plus_1_times();
    test_inc_dec_16bit_no_flags();
    test_adr_preserves_flags();
    test_sjp_rtn_stack_roundtrip();
    test_psh_pop_pair_roundtrip();
    test_hlt_and_interrupt_wake();
    test_wake_from_halt_ignores_ie();
    test_off_instruction_powers_down_until_power_on();
    test_rti_restores_flags_and_pc();
    test_trace_ring_and_drain();
    test_trace_ring_overflow_accounting();
    test_trace_ring_peek_does_not_consume();
    test_breakpoint_halts_step();
    test_illegal_opcode_is_distinguishable_from_nop();
    test_flag_cost_gating_no_trace_by_default();
    test_memory_open_bus_and_ram_regions();
    test_memory_display_ram_mirroring();
    test_memory_pc1500a_system_ram_has_no_aliasing();
    test_memory_pc1500_system_ram_has_aliasing();
    test_memory_pc1500_narrower_user_ram();
    test_rom_load_and_write_ignored();
    test_keyboard_scan_active_low_matrix();
    test_keyboard_name_lookup();
    test_io_chip_keyboard_wiring_through_memory();
    test_rtc_tp_rate_and_gating();
    test_rtc_tp_phase_free_runs_across_configures();
    test_rtc_if_does_not_clear_on_read();
    test_pc1500_ram_powerup_reset_and_all_reset();
    test_rtc_opb_and_if_never_disagree_within_one_poll();
    test_rtc_calendar_seed_and_read();
    test_preset_syncclock_reseeds_rtc();
    test_rtc_calendar_set_via_shift_and_commit();
    test_rtc_calendar_time_set_straight_to_time_read();
    test_rtc_calendar_advances_one_hz_with_bcd_and_month_carry();
    test_rtc_seed_millisecond_aligns_next_tick();
    test_rtc_calendar_reset_is_deterministic();
    test_machine_seedclock_derives_day_of_week();
    test_on_key_press_sets_break_flag();
    test_on_key_wakes_a_halted_cpu_with_timer_stopped();
    test_on_key_powers_a_powered_off_machine_back_on();
    test_machine_presskey_releasekey_named_api();
    test_machine_enqueuekey_paces_press_hold_release_gap();
    test_machine_enqueuekey_capacity_drops_newest();
    test_machine_enqueuekey_ignores_unmapped_name();
    test_display_pixel_decode();
    test_display_status_icons();
    test_boot_smoke_real_rom();
    test_pc1500_reset_releases_held_keys();

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    int connectorFailures = run_connector_tests();
    int ce155Failures = run_ce155_tests();
    int ce1638Failures = run_ce1638_tests();
    int ce502bFailures = run_ce502b_tests();
    int ce163fFailures = run_ce163f_tests();
    int memoryCardFailures = run_memory_card_tests();
    int batteryCardInstanceFailures = run_battery_card_instance_tests();
    int presetFailures = run_preset_tests();
    int basicTyperFailures = run_basictyper_tests();
    int presetLoaderTraceFailures = run_presetloader_trace_tests();
    int pc1600BasicTyperFailures = run_pc1600_basictyper_tests();
    int pc1600BankFailures = run_pc1600_bank_tests();
    int sc7852Failures = run_sc7852_tests();
    int tc8576fFailures = run_tc8576f_tests();
    int lh5803Failures = run_lh5803_tests();
    int pc1600MachineFailures = run_pc1600_machine_tests();
    int pc1600Phase54Failures = run_pc1600_phase54_tests();
    int pc1600KeyboardDisplayFailures = run_pc1600_keyboard_display_tests();
    int pc1600SlotRamFailures = run_pc1600_slot_ram_tests();
    int pc1600SlotModuleFailures = run_pc1600_slot_module_tests();
    int pc1600PresetFailures = run_pc1600_preset_tests();
    int ce1600pFailures = run_ce1600p_tests();
    int ce1600fFailures = run_ce1600f_tests();
    int ce150Failures = run_ce150_tests();
    int pc1600Ce150Failures = run_pc1600_ce150_tests();
    int ce158Failures = run_ce158_tests();
    int basicBinaryImageFailures = run_basic_binary_image_tests();
    int basicFastLoaderFailures = run_basic_fastloader_tests();
    int pc1600BasicLoaderFailures = run_pc1600_basicloader_tests();
    int basicProgramSourceFailures = run_basic_program_source_tests();
    int pc1600ProgramPlacementFailures = run_pc1600_program_placement_tests();
    int pc1600MachineImageFailures = run_pc1600_machine_image_tests();
    int piezoSamplerFailures = run_piezo_sampler_tests();
    int keyPasteFailures = run_key_paste_tests();
    int lcdScreenshotFailures = run_lcd_screenshot_tests();
    int machineCodeFileFailures = run_machine_code_file_tests();
    int disasmFailures = run_disasm_tests();
    int debugTargetFailures = run_debug_target_tests();
    int listingFailures = run_listing_tests();
    int runControlFailures = run_run_control_tests();
    return (g_fail == 0 && machineCodeFileFailures == 0 && disasmFailures == 0 && debugTargetFailures == 0 && listingFailures == 0 && runControlFailures == 0 && piezoSamplerFailures == 0 && keyPasteFailures == 0 && lcdScreenshotFailures == 0 && basicBinaryImageFailures == 0 && basicFastLoaderFailures == 0 && pc1600BasicLoaderFailures == 0 && basicProgramSourceFailures == 0 && pc1600ProgramPlacementFailures == 0 && pc1600MachineImageFailures == 0 && connectorFailures == 0 && ce155Failures == 0 && ce1638Failures == 0 && ce502bFailures == 0 &&
            ce163fFailures == 0 && memoryCardFailures == 0 && batteryCardInstanceFailures == 0 &&
            presetFailures == 0 &&
            basicTyperFailures == 0 && pc1600BasicTyperFailures == 0 &&
            presetLoaderTraceFailures == 0 && pc1600BankFailures == 0 && sc7852Failures == 0 &&
            tc8576fFailures == 0 &&
            lh5803Failures == 0 && pc1600MachineFailures == 0 && pc1600Phase54Failures == 0 &&
            pc1600KeyboardDisplayFailures == 0 && pc1600SlotRamFailures == 0 &&
            pc1600SlotModuleFailures == 0 && pc1600PresetFailures == 0 && ce1600pFailures == 0 &&
            ce1600fFailures == 0 &&
            ce150Failures == 0 && pc1600Ce150Failures == 0 && ce158Failures == 0)
               ? 0
               : 1;
}
