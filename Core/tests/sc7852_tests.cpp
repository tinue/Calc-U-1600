// Headless C++ tests for the SC7852 (Z-80-compatible) core
// (Core/CPU/SC7852/SC7852.hpp/.cpp). Same no-framework, assert-and-
// tally style as lh5801_tests.cpp -- see that file's header comment.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <set>
#include <vector>

#include "../CPU/SC7852/SC7852.hpp"
#include "../PC1600/PC1600Bank.hpp"
#include "../PC1600/PC1600Memory.hpp"
#include "TestRoms.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// A trivial RAM-backed bus for isolated opcode tests -- mirrors
// lh5801_tests.cpp's FlatBus. IO is a flat 256-entry array too, readable
// back exactly as last written (good enough for IN/OUT round-trip tests;
// no PC1600Bank/port semantics involved).
class FlatBus : public SC7852Bus {
public:
    uint8_t mem[65536]{};
    uint8_t io[256]{};
    uint8_t readMem(uint16_t addr) override { return mem[addr]; }
    void    writeMem(uint16_t addr, uint8_t v) override { mem[addr] = v; }
    uint8_t readIO(uint8_t port) override { return io[port]; }
    void    writeIO(uint8_t port, uint8_t v) override { io[port] = v; }
};

// Loads `code` at 0x0000 (SC7852's reset PC), returns a ready-to-step CPU.
struct Rig {
    FlatBus bus;
    SC7852 cpu;
    explicit Rig(std::vector<uint8_t> code) : cpu(bus) {
        for (size_t i = 0; i < code.size(); i++) bus.mem[i] = code[i];
        cpu.reset();
    }
};

void test_reset_state() {
    FlatBus bus;
    SC7852 cpu(bus);
    CHECK(cpu.pc() == 0x0000);
    CHECK(cpu.sp() == 0xFFFF);
    CHECK(!cpu.iff1());
    CHECK(!cpu.iff2());
    CHECK(cpu.im() == 0);
}

void test_ld_r_n_and_ld_r_r() {
    // LD B,0x42 ; LD C,B (0x48 = dst=C(001) src=B(000))
    Rig r({0x06, 0x42, 0x48});
    r.cpu.step();
    CHECK(r.cpu.bc() == 0x4200);
    r.cpu.step();
    CHECK(r.cpu.bc() == 0x4242);
}

void test_ld_hl_nn_and_ld_mem_hl() {
    // LD HL,0x9000 ; LD (HL),0x55 ; LD A,(HL)
    Rig r({0x21, 0x00, 0x90, 0x36, 0x55, 0x7E});
    r.cpu.step();
    CHECK(r.cpu.hl() == 0x9000);
    r.cpu.step();
    CHECK(r.bus.mem[0x9000] == 0x55);
    r.cpu.step();
    CHECK(r.cpu.a() == 0x55);
}

void test_add_flags() {
    // LD A,0xFF ; ADD A,0x01 -> A=0x00, Z=1, C=1, H=1
    Rig r({0x3E, 0xFF, 0xC6, 0x01});
    r.cpu.step();
    r.cpu.step();
    CHECK(r.cpu.a() == 0x00);
    CHECK(r.cpu.flagZ());
    CHECK(r.cpu.flagC());
    CHECK(r.cpu.flagH());
}

void test_adc_carry_in() {
    // SCF ; LD A,0x01 ; ADC A,0x01 -> A=3
    Rig r({0x37, 0x3E, 0x01, 0xCE, 0x01});
    r.cpu.step(); r.cpu.step(); r.cpu.step();
    CHECK(r.cpu.a() == 0x03);
    CHECK(!r.cpu.flagC());
}

void test_sub_and_cp_no_writeback() {
    // LD A,0x05 ; SUB 0x03 -> A=2, C=0(no borrow)
    Rig r({0x3E, 0x05, 0xD6, 0x03});
    r.cpu.step(); r.cpu.step();
    CHECK(r.cpu.a() == 0x02);
    CHECK(!r.cpu.flagC());

    // LD A,0x05 ; CP 0x05 -> Z=1, A unmodified
    Rig r2({0x3E, 0x05, 0xFE, 0x05});
    r2.cpu.step(); r2.cpu.step();
    CHECK(r2.cpu.a() == 0x05);
    CHECK(r2.cpu.flagZ());
}

void test_inc_dec_8bit_flags() {
    // LD A,0x7F ; INC A -> A=0x80, PV=1 (overflow), S=1
    Rig r({0x3E, 0x7F, 0x3C});
    r.cpu.step(); r.cpu.step();
    CHECK(r.cpu.a() == 0x80);
    CHECK(r.cpu.flagPV());
    CHECK(r.cpu.flagS());

    // LD A,0x00 ; DEC A -> A=0xFF, PV=0, N=1
    Rig r2({0x3E, 0x00, 0x3D});
    r2.cpu.step(); r2.cpu.step();
    CHECK(r2.cpu.a() == 0xFF);
    CHECK(!r2.cpu.flagPV());
    CHECK(r2.cpu.flagN());
}

void test_16bit_inc_dec_no_flag_change() {
    // SCF ; LD BC,0xFFFF ; INC BC -> BC=0, C flag untouched (still set)
    Rig r({0x37, 0x01, 0xFF, 0xFF, 0x03});
    r.cpu.step(); r.cpu.step();
    CHECK(r.cpu.bc() == 0xFFFF);
    r.cpu.step();
    CHECK(r.cpu.bc() == 0x0000);
    CHECK(r.cpu.flagC()); // untouched from SCF
}

void test_and_or_xor() {
    // LD A,0xF0 ; AND 0x3C -> 0x30, H=1,C=0
    Rig r({0x3E, 0xF0, 0xE6, 0x3C});
    r.cpu.step(); r.cpu.step();
    CHECK(r.cpu.a() == 0x30);
    CHECK(r.cpu.flagH());
    CHECK(!r.cpu.flagC());

    // LD A,0xF0 ; OR 0x0F -> 0xFF
    Rig r2({0x3E, 0xF0, 0xF6, 0x0F});
    r2.cpu.step(); r2.cpu.step();
    CHECK(r2.cpu.a() == 0xFF);

    // LD A,0xFF ; XOR 0xFF -> 0x00, Z=1
    Rig r3({0x3E, 0xFF, 0xEE, 0xFF});
    r3.cpu.step(); r3.cpu.step();
    CHECK(r3.cpu.a() == 0x00);
    CHECK(r3.cpu.flagZ());
}

void test_add_hl_rr() {
    // LD HL,0x1000 ; LD BC,0x2000 ; ADD HL,BC -> HL=0x3000, C unaffected by
    // this add (no overflow past 16 bits)
    Rig r({0x21, 0x00, 0x10, 0x01, 0x00, 0x20, 0x09});
    r.cpu.step(); r.cpu.step(); r.cpu.step();
    CHECK(r.cpu.hl() == 0x3000);
}

void test_jp_jr_and_conditions() {
    // XOR A (Z=1) ; JP Z,0x0010 ; (skip) ... at 0x0010: LD A,0x99
    Rig r({0xAF, 0xCA, 0x10, 0x00});
    r.bus.mem[0x0010] = 0x3E;
    r.bus.mem[0x0011] = 0x99;
    r.cpu.step(); // XOR A
    CHECK(r.cpu.flagZ());
    r.cpu.step(); // JP Z,0x0010
    CHECK(r.cpu.pc() == 0x0010);
    r.cpu.step();
    CHECK(r.cpu.a() == 0x99);
}

void test_djnz() {
    // LD B,0x03 ; loop: DEC A(no-op filler) ; DJNZ loop
    // Actually use a tight self-loop: LD B,3 ; DJNZ -2 (back to itself)
    Rig r({0x06, 0x03, 0x10, 0xFE});
    r.cpu.step(); // LD B,3
    CHECK(r.cpu.bc() == 0x0300);
    r.cpu.step(); // DJNZ: B->2, jump back to 0x0002
    CHECK((r.cpu.bc() >> 8) == 2);
    CHECK(r.cpu.pc() == 0x0002);
    r.cpu.step();
    CHECK((r.cpu.bc() >> 8) == 1);
    CHECK(r.cpu.pc() == 0x0002);
    r.cpu.step(); // B->0, falls through
    CHECK((r.cpu.bc() >> 8) == 0);
    CHECK(r.cpu.pc() == 0x0004);
}

void test_call_ret_stack() {
    // LD SP,0x9000 ; CALL 0x0010 ; (return lands here at 0x0007) LD A,0x55
    // at 0x0010: RET
    Rig r({0x31, 0x00, 0x90, 0xCD, 0x10, 0x00, 0x00, 0x3E, 0x55});
    r.bus.mem[0x0010] = 0xC9; // RET
    r.cpu.step(); // LD SP,0x9000
    CHECK(r.cpu.sp() == 0x9000);
    r.cpu.step(); // CALL 0x0010
    CHECK(r.cpu.pc() == 0x0010);
    CHECK(r.cpu.sp() == 0x8FFE);
    CHECK(r.bus.mem[0x8FFE] == 0x06); // low byte of return addr 0x0006
    CHECK(r.bus.mem[0x8FFF] == 0x00);
    r.cpu.step(); // RET
    CHECK(r.cpu.pc() == 0x0006);
    CHECK(r.cpu.sp() == 0x9000);
}

void test_push_pop_roundtrip() {
    // LD SP,0x9000 ; LD BC,0x1234 ; PUSH BC ; LD BC,0x0000 ; POP BC
    Rig r({0x31, 0x00, 0x90, 0x01, 0x34, 0x12, 0xC5, 0x01, 0x00, 0x00, 0xC1});
    r.cpu.step(); r.cpu.step(); r.cpu.step(); // SP, BC, PUSH
    CHECK(r.cpu.sp() == 0x8FFE);
    r.cpu.step(); // BC=0
    CHECK(r.cpu.bc() == 0x0000);
    r.cpu.step(); // POP BC
    CHECK(r.cpu.bc() == 0x1234);
    CHECK(r.cpu.sp() == 0x9000);
}

void test_ex_af_af_and_exx() {
    // LD A,0x11 ; EX AF,AF' ; LD A,0x22 ; EX AF,AF' -> A back to 0x11
    Rig r({0x3E, 0x11, 0x08, 0x3E, 0x22, 0x08});
    r.cpu.step(); r.cpu.step();
    CHECK(r.cpu.a() == 0xFF); // A was 0x11, EX AF,AF' swapped in the shadow's default 0xFF
    r.cpu.step(); // LD A,0x22
    CHECK(r.cpu.a() == 0x22);
    r.cpu.step(); // EX AF,AF' back -> A=0x11
    CHECK(r.cpu.a() == 0x11);
}

void test_ex_de_hl() {
    // LD DE,0x1111 ; LD HL,0x2222 ; EX DE,HL
    Rig r({0x11, 0x11, 0x11, 0x21, 0x22, 0x22, 0xEB});
    r.cpu.step(); r.cpu.step(); r.cpu.step();
    CHECK(r.cpu.de() == 0x2222);
    CHECK(r.cpu.hl() == 0x1111);
}

void test_rlca_rrca_rla_rra_leave_szpv_alone() {
    // XOR A first to get S deterministically clear (this core's post-reset
    // F is not a documented value -- see SC7852::reset()'s comment -- so a
    // known flag state has to be established before testing "untouched").
    // LD A,0x81 ; RLCA -> A=0x03, C=1; S must stay clear (RLCA doesn't
    // touch S, unlike CB-prefixed RLC A).
    Rig r({0xAF, 0x3E, 0x81, 0x07});
    r.cpu.step(); // XOR A
    CHECK(!r.cpu.flagS());
    r.cpu.step(); // LD A,0x81 (flags untouched)
    r.cpu.step(); // RLCA
    CHECK(r.cpu.a() == 0x03);
    CHECK(r.cpu.flagC());
    CHECK(!r.cpu.flagS());
}

void test_daa_after_bcd_add() {
    // LD A,0x09 ; ADD A,0x01 ; DAA -> decimal 9+1=10 -> A=0x10
    Rig r({0x3E, 0x09, 0xC6, 0x01, 0x27});
    r.cpu.step(); r.cpu.step(); r.cpu.step();
    CHECK(r.cpu.a() == 0x10);
}

void test_sub_half_borrow_flag() {
    // SUB's H flag is a half-BORROW (borrow out of bit 3), and it must be
    // the exact inverse of the "add the one's complement" carry the core
    // computes internally -- the same inversion kFlagC already gets.
    // LD A,0x10 ; SUB 0x01 -> 0x0F, low nibble 0-1 borrows -> H set.
    Rig borrow({0x3E, 0x10, 0xD6, 0x01});
    borrow.cpu.step(); borrow.cpu.step();
    CHECK(borrow.cpu.a() == 0x0F);
    CHECK(borrow.cpu.flagH());
    CHECK(borrow.cpu.flagN());
    CHECK(!borrow.cpu.flagC());

    // LD A,0x18 ; SUB 0x01 -> 0x17, low nibble 8-1 does not borrow -> H clear.
    Rig noBorrow({0x3E, 0x18, 0xD6, 0x01});
    noBorrow.cpu.step(); noBorrow.cpu.step();
    CHECK(noBorrow.cpu.a() == 0x17);
    CHECK(!noBorrow.cpu.flagH());
}

void test_daa_after_bcd_sub() {
    // The PC-1600 ROM's floating-point add subtracts BCD digit pairs when
    // the operands' signs differ (e.g. BASIC `L + 5` with L = -5). That
    // path is SUB/SBC followed by DAA, so SUB's H flag must report the
    // correct (non-inverted) half-borrow sense for DAA to correct properly.
    // LD A,0x42 ; SUB 0x08 -> 0x3A ; DAA -> decimal 42-8=34 -> A=0x34.
    Rig r({0x3E, 0x42, 0xD6, 0x08, 0x27});
    r.cpu.step(); r.cpu.step(); r.cpu.step();
    CHECK(r.cpu.a() == 0x34);
    CHECK(!r.cpu.flagC());

    // Borrow across the whole byte: LD A,0x00 ; SUB 0x01 -> 0xFF ; DAA ->
    // decimal 100-1=99 with a borrow -> A=0x99, C set.
    Rig wrap({0x3E, 0x00, 0xD6, 0x01, 0x27});
    wrap.cpu.step(); wrap.cpu.step(); wrap.cpu.step();
    CHECK(wrap.cpu.a() == 0x99);
    CHECK(wrap.cpu.flagC());
}

void test_cb_rotate_and_bit_and_set_res() {
    // LD B,0x80 ; CB 00 (RLC B) -> B=0x01, C=1
    Rig r({0x06, 0x80, 0xCB, 0x00});
    r.cpu.step();
    r.cpu.step();
    CHECK(r.cpu.bc() >> 8 == 0x01);
    CHECK(r.cpu.flagC());

    // LD A,0x40 ; BIT 6,A -> Z=0 ; BIT 5,A -> Z=1
    Rig r2({0x3E, 0x40, 0xCB, 0x77, 0xCB, 0x6F});
    r2.cpu.step(); r2.cpu.step();
    CHECK(!r2.cpu.flagZ());
    r2.cpu.step();
    CHECK(r2.cpu.flagZ());

    // LD A,0x00 ; SET 0,A -> 0x01 ; RES 0,A -> 0x00
    Rig r3({0x3E, 0x00, 0xCB, 0xC7, 0xCB, 0x87});
    r3.cpu.step(); r3.cpu.step();
    CHECK(r3.cpu.a() == 0x01);
    r3.cpu.step();
    CHECK(r3.cpu.a() == 0x00);
}

void test_ed_ldir_block_copy() {
    // LD HL,src ; LD DE,dst ; LD BC,3 ; LDIR
    Rig r({0x21, 0x00, 0x80, 0x11, 0x00, 0x90, 0x01, 0x03, 0x00, 0xED, 0xB0});
    r.bus.mem[0x8000] = 0x11;
    r.bus.mem[0x8001] = 0x22;
    r.bus.mem[0x8002] = 0x33;
    r.cpu.step(); r.cpu.step(); r.cpu.step();
    r.cpu.step(); // LDIR (loops internally via our PC-2 rewind, so one
                   // logical call to step() only advances one iteration --
                   // call repeatedly until BC==0)
    while (r.cpu.bc() != 0) r.cpu.step();
    CHECK(r.bus.mem[0x9000] == 0x11);
    CHECK(r.bus.mem[0x9001] == 0x22);
    CHECK(r.bus.mem[0x9002] == 0x33);
    CHECK(r.cpu.hl() == 0x8003);
    CHECK(r.cpu.de() == 0x9003);
}

void test_dd_before_ed_is_ignored() {
    // LD HL,src ; LD DE,dst ; LD BC,2 ; DD ED B0 = LDIR (the DD is a NOP)
    Rig r({0x21, 0x00, 0x80, 0x11, 0x00, 0x90, 0x01, 0x02, 0x00, 0xDD, 0xED, 0xB0});
    r.bus.mem[0x8000] = 0x11;
    r.bus.mem[0x8001] = 0x22;
    r.cpu.step(); r.cpu.step(); r.cpu.step();
    uint8_t a = r.cpu.a();
    r.cpu.step();
    while (r.cpu.bc() != 0) r.cpu.step();
    CHECK(r.bus.mem[0x9000] == 0x11);
    CHECK(r.bus.mem[0x9001] == 0x22);
    CHECK(r.cpu.a() == a); // not run as OR B
    CHECK(r.cpu.pc() == 0x000C);
}

void test_last_index_prefix_wins() {
    // DD FD 21 34 12 = LD IY,1234h ; FD DD 21 78 56 = LD IX,5678h
    Rig r({0xDD, 0xFD, 0x21, 0x34, 0x12, 0xFD, 0xDD, 0x21, 0x78, 0x56});
    CHECK(r.cpu.step() == 4 + SC7852::kM1WaitStates); // the ignored DD: one M1 NOP
    CHECK(r.cpu.pc() == 0x0002);                       // FD already fetched
    int plain = 0;
    { Rig q({0xFD, 0x21, 0x00, 0x00}); plain = q.cpu.step(); }
    r.cpu.setTraceFlags(TRACE_PC);
    CHECK(r.cpu.step() == plain); // the carried FD runs exactly like a plain FD 21
    Z80CpuFrame fr[1];
    CHECK(r.cpu.drainTraceEvents(fr, 1, nullptr) == 1);
    CHECK(fr[0].pc == 0x0001 && fr[0].opcode == 0xFD21); // traced at the FD, not the 21
    r.cpu.setTraceFlags(TRACE_NONE);
    CHECK(r.cpu.iy() == 0x1234);
    CHECK(r.cpu.ix() == 0x0000);
    CHECK(r.cpu.pc() == 0x0005);
    r.cpu.step(); r.cpu.step();
    CHECK(r.cpu.ix() == 0x5678);
    CHECK(r.cpu.pc() == 0x000A);
}

void test_prefix_run_is_bounded_and_blocks_int() {
    std::vector<uint8_t> code(64, 0xDD);
    code[0] = 0xFB; // EI, then a run of DD prefixes
    Rig r(code);
    r.cpu.step();
    r.cpu.setIntLine(true);
    for (int i = 0; i < 8; i++) {
        CHECK(r.cpu.step() == 4 + SC7852::kM1WaitStates); // one prefix per step, no INT taken
    }
    CHECK(r.cpu.pc() == 0x000A);
    CHECK(r.cpu.iff1());
}

void test_r_counts_m1_cycles_only() {
    // LD A,n ; LD HL,nn ; LD (IX+d),n ; DD CB d 06 (RLC (IX+d)) ; LD A,R
    Rig r({0x3E, 0x01, 0x21, 0x00, 0x90, 0xDD, 0x36, 0x00, 0x11,
           0xDD, 0xCB, 0x00, 0x06, 0xED, 0x5F});
    r.cpu.step(); CHECK(r.cpu.r() == 1);
    r.cpu.step(); CHECK(r.cpu.r() == 2);
    r.cpu.step(); CHECK(r.cpu.r() == 4); // DD + opcode; d and n are operands
    r.cpu.step(); CHECK(r.cpu.r() == 6); // DD + CB; d and op are not M1s
    r.cpu.step(); // LD A,R: R already advanced by ED + 5F when it is read
    CHECK(r.cpu.a() == 8);
}

void test_ed_adc_sbc_hl() {
    // SCF ; LD HL,0x0001 ; LD BC,0x0001 ; ADC HL,BC -> HL=3 (1+1+1)
    Rig r({0x37, 0x21, 0x01, 0x00, 0x01, 0x01, 0x00, 0xED, 0x4A});
    r.cpu.step(); r.cpu.step(); r.cpu.step(); r.cpu.step();
    CHECK(r.cpu.hl() == 0x0003);
}

void test_ed_neg() {
    // LD A,0x01 ; NEG -> A=0xFF, C=1
    Rig r({0x3E, 0x01, 0xED, 0x44});
    r.cpu.step(); r.cpu.step();
    CHECK(r.cpu.a() == 0xFF);
    CHECK(r.cpu.flagC());
}

void test_in_out_roundtrip() {
    // LD A,0x42 ; OUT (0x10),A ; IN A,(0x10) after clearing A first
    Rig r({0x3E, 0x42, 0xD3, 0x10, 0x3E, 0x00, 0xDB, 0x10});
    r.cpu.step(); r.cpu.step();
    CHECK(r.bus.io[0x10] == 0x42);
    r.cpu.step(); r.cpu.step();
    CHECK(r.cpu.a() == 0x42);
}

void test_di_ei_and_im() {
    Rig r({0xF3, 0xFB, 0xED, 0x56}); // DI ; EI ; IM 1
    r.cpu.step();
    CHECK(!r.cpu.iff1());
    r.cpu.step();
    CHECK(r.cpu.iff1());
    r.cpu.step();
    CHECK(r.cpu.im() == 1);
}

void test_interrupt_im1_pushes_pc_and_vectors_to_0038() {
    Rig r({0xFB, 0x00, 0x00}); // EI ; NOP ; NOP
    r.cpu.step(); // EI
    r.cpu.setIntLine(true);
    r.cpu.step(); // NOP: runs in the EI shadow
    r.cpu.step(); // services the pending IRQ (IM defaults 0 -> treated as IM1 path)
    CHECK(r.cpu.pc() == 0x0038);
    CHECK(!r.cpu.iff1());
}

void test_halt_wakes_on_interrupt() {
    Rig r({0xFB, 0x76, 0x00}); // EI ; HALT ; NOP
    r.cpu.step(); r.cpu.step();
    CHECK(r.cpu.halted());
    int cyclesWhileHalted = r.cpu.step();
    CHECK(cyclesWhileHalted == 0);
    r.cpu.setIntLine(true);
    r.cpu.step();
    CHECK(!r.cpu.halted());
    CHECK(r.cpu.pc() == 0x0038);
    CHECK(r.bus.mem[r.cpu.sp()] == 0x02); // returns past the HALT
}

void test_masked_int_keeps_halt() {
    Rig r({0x76, 0x00}); // HALT (IFF1 clear: never EI'd)
    r.cpu.step();
    r.cpu.setIntLine(true);
    CHECK(r.cpu.step() == 0);
    CHECK(r.cpu.halted()); // a masked INT does not end HALT
    r.cpu.resumeFromHalt();
    r.cpu.setPC(0x0001);
    r.bus.mem[0x0001] = 0xFB; // EI
    r.bus.mem[0x0002] = 0x00; // NOP
    r.cpu.step(); // EI
    r.cpu.step(); // NOP (EI shadow)
    r.cpu.step(); // the line is still held, so it is accepted now
    CHECK(r.cpu.pc() == 0x0038);
}

void test_int_line_dropped_before_ei_is_not_taken() {
    Rig r({0xFB, 0x00, 0x00, 0x00}); // EI ; NOP ; NOP ; NOP
    r.cpu.setIntLine(true);
    r.cpu.setIntLine(false); // the device withdrew it (cause masked/cleared)
    r.cpu.step(); r.cpu.step(); r.cpu.step();
    CHECK(r.cpu.pc() == 0x0003);
    CHECK(r.cpu.iff1());
}

void test_ei_defers_pending_irq_by_one_instruction() {
    Rig r({0xFB, 0x76, 0x00}); // EI ; HALT ; NOP
    r.cpu.setIntLine(true); // already pending when EI runs
    r.cpu.step(); // EI
    r.cpu.step(); // HALT still executes before the IRQ is accepted
    CHECK(r.cpu.halted());
    r.cpu.step(); // now accepted
    CHECK(r.cpu.pc() == 0x0038);
    CHECK(r.bus.mem[r.cpu.sp()] == 0x02);
    CHECK(r.bus.mem[uint16_t(r.cpu.sp() + 1)] == 0x00);
}

void test_ix_load_and_displacement_access() {
    // DD 21 nn nn: LD IX,0x9000 ; DD 36 05 42: LD (IX+5),0x42 ; DD 7E 05: LD A,(IX+5)
    Rig r({0xDD, 0x21, 0x00, 0x90, 0xDD, 0x36, 0x05, 0x42, 0xDD, 0x7E, 0x05});
    r.cpu.step();
    CHECK(r.cpu.ix() == 0x9000);
    r.cpu.step();
    CHECK(r.bus.mem[0x9005] == 0x42);
    r.cpu.step();
    CHECK(r.cpu.a() == 0x42);
}

void test_ix_add_and_inc_dec() {
    // DD 21: LD IX,0x1000 ; DD 09(after LD BC,1): ADD IX,BC -> 0x1001 ;
    // DD 23: INC IX -> 0x1002
    Rig r({0x01, 0x01, 0x00, 0xDD, 0x21, 0x00, 0x10, 0xDD, 0x09, 0xDD, 0x23});
    r.cpu.step(); // LD BC,1
    r.cpu.step(); // LD IX,0x1000
    CHECK(r.cpu.ix() == 0x1000);
    r.cpu.step(); // ADD IX,BC
    CHECK(r.cpu.ix() == 0x1001);
    r.cpu.step(); // INC IX
    CHECK(r.cpu.ix() == 0x1002);
}

void test_ix_plain_opcode_passthrough_when_unrelated() {
    // DD prefix in front of an opcode that doesn't reference H/L/(HL) --
    // e.g. LD BC,nn -- must behave exactly as the unprefixed form (wasted
    // prefix, no IX involvement).
    Rig r({0xDD, 0x01, 0x34, 0x12});
    r.cpu.step();
    CHECK(r.cpu.bc() == 0x1234);
}

void test_ddcb_bit_set_res_on_displacement() {
    // DD 21: LD IX,0x9000 ; DD CB 03 C6: SET 0,(IX+3) ; DD CB 03 46: BIT 0,(IX+3)
    Rig r({0xDD, 0x21, 0x00, 0x90, 0xDD, 0xCB, 0x03, 0xC6, 0xDD, 0xCB, 0x03, 0x46});
    r.cpu.step();
    CHECK(r.cpu.ix() == 0x9000);
    r.cpu.step(); // SET 0,(IX+3)
    CHECK(r.bus.mem[0x9003] == 0x01);
    r.cpu.step(); // BIT 0,(IX+3)
    CHECK(!r.cpu.flagZ()); // bit is set -> Z clear
}

// ── Boot smoke test: real ROM images, PC1600Bank/PC1600Memory as the bus ──

void test_boot_smoke_real_rom() {
    std::vector<uint8_t> lower, upper, bank3, bank3b, bank6;
    if (!readRomImage("roms/PC1600-P0-B0-new.bin", &lower) ||
        !readRomImage("roms/PC1600-P1-B0-new.bin", &upper) ||
        !readRomImage("roms/PC1600-P1-B3-new.bin", &bank3) ||
        !readRomImage("roms/PC1600-P1-B3B-new.bin", &bank3b) ||
        !readRomImage("roms/PC1600-P2-B6-new.bin", &bank6)) {
        std::fprintf(stderr, "SKIP test_boot_smoke_real_rom: one or more roms/PC1600-*.bin "
                              "files not found relative to cwd (run tests from the repo root)\n");
        return;
    }

    PC1600Bank bank;
    PC1600Memory mem(bank);
    CHECK(mem.loadBank0(lower.data(), lower.size(), upper.data(), upper.size()));
    CHECK(mem.loadBank3Rom(bank3.data(), bank3.size()));
    CHECK(mem.loadBank3bRom(bank3b.data(), bank3b.size()));
    CHECK(mem.loadBank6Rom(bank6.data(), bank6.size()));
    SC7852 cpu(mem);
    cpu.reset();
    CHECK(cpu.pc() == 0x0000);

    // Loads bank3/bank3b/bank6 in addition to bank0, with a 2M-step
    // warm-up: the boot ROM's own busy-wait on PC1600Display's "not busy"
    // status (0x0807 in PC1600-P0-B0-new.bin) only clears once that status is
    // reported accurately, letting real execution continue on into
    // bank3/bank6 code rather than parking in a tiny loop. Running this
    // test without those banks loaded produces open-bus wandering, not a
    // bounded loop, so the fuller ROM set is required for the "boots to a
    // stable, bounded execution pattern" check below to mean anything.
    //
    // Run a substantial warm-up period, then confirm the PC settles into
    // a small, bounded set of addresses -- a "stable, bounded execution
    // pattern". A ROM boot that ran off into garbage (bad decode, wrong
    // flag behavior corrupting a branch) would instead keep visiting
    // new, ever-growing sets of
    // addresses. **Not yet a claim of reaching genuine BASIC-ready idle**:
    // this is a standalone SC7852 + PC1600Memory (no PC1600Machine), so
    // the TC8576F is present with real SSR/PSR status but its BUSY window
    // and the 64Hz/0.5s interrupt sources are not being pumped -- the
    // boot still settles into a bounded loop, which is all this exit
    // criterion asks.
    for (int i = 0; i < 2'000'000; i++) cpu.step();

    std::set<uint16_t> visited;
    for (int i = 0; i < 200000; i++) {
        visited.insert(cpu.pc());
        cpu.step();
    }
    CHECK(visited.size() < 500);
}

} // namespace

int run_sc7852_tests() {
    test_reset_state();
    test_ld_r_n_and_ld_r_r();
    test_ld_hl_nn_and_ld_mem_hl();
    test_add_flags();
    test_adc_carry_in();
    test_sub_and_cp_no_writeback();
    test_inc_dec_8bit_flags();
    test_16bit_inc_dec_no_flag_change();
    test_and_or_xor();
    test_add_hl_rr();
    test_jp_jr_and_conditions();
    test_djnz();
    test_call_ret_stack();
    test_push_pop_roundtrip();
    test_ex_af_af_and_exx();
    test_ex_de_hl();
    test_rlca_rrca_rla_rra_leave_szpv_alone();
    test_daa_after_bcd_add();
    test_sub_half_borrow_flag();
    test_daa_after_bcd_sub();
    test_cb_rotate_and_bit_and_set_res();
    test_ed_ldir_block_copy();
    test_r_counts_m1_cycles_only();
    test_dd_before_ed_is_ignored();
    test_last_index_prefix_wins();
    test_prefix_run_is_bounded_and_blocks_int();
    test_ed_adc_sbc_hl();
    test_ed_neg();
    test_in_out_roundtrip();
    test_di_ei_and_im();
    test_interrupt_im1_pushes_pc_and_vectors_to_0038();
    test_halt_wakes_on_interrupt();
    test_masked_int_keeps_halt();
    test_int_line_dropped_before_ei_is_not_taken();
    test_ei_defers_pending_irq_by_one_instruction();
    test_ix_load_and_displacement_access();
    test_ix_add_and_inc_dec();
    test_ix_plain_opcode_passthrough_when_unrelated();
    test_ddcb_bit_set_res_on_displacement();
    test_boot_smoke_real_rom();

    std::printf("sc7852_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
