// Headless C++ tests for the dual-CPU facade
// (Core/PC1600/PC1600Machine.hpp/.cpp, PC1600BusArbiter.hpp). Same
// no-framework, assert-and-tally style as lh5801_tests.cpp -- see that
// file's header comment.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <deque>
#include <initializer_list>
#include <string>
#include <vector>

#include "../PC1600/PC1600Machine.hpp"
#include "TestCards.hpp"
#include "TestRoms.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

std::vector<uint8_t> makeBank(uint8_t fillByte) {
    return std::vector<uint8_t>(PC1600Memory::kBankSize, fillByte);
}

void test_reset_starts_on_sc7852() {
    PC1600Machine m;
    m.reset();
    CHECK(m.sc7852Owns());
    CHECK(m.sc7852().pc() == 0x0000);
}

void test_sc7852_to_lh5803_handoff() {
    PC1600Machine m;
    // Bank 0 lower/upper (pages A/B bank 0): OUT (38H),A ; HALT at 0x0000.
    std::vector<uint8_t> lower = makeBank(0x00);
    std::vector<uint8_t> upper = makeBank(0x00);
    lower[0] = 0xD3; lower[1] = 0x38; // OUT (38H),A
    lower[2] = 0x76;                  // HALT
    CHECK(m.loadBank0(lower.data(), lower.size(), upper.data(), upper.size()));

    // LH5803 ROM: NOP at its own reset target, just needs to be present so
    // its step() after the handoff doesn't dereference an unloaded ROM.
    std::vector<uint8_t> lh5803Rom(16384, 0x00);
    lh5803Rom[16384 - 2] = 0xC0; // reset vector -> 0xC000 (LH5803Bus.hpp's own valid range)
    lh5803Rom[16384 - 1] = 0x00;
    CHECK(m.loadLH5803Rom(lh5803Rom.data(), lh5803Rom.size()));

    m.reset();
    CHECK(m.sc7852Owns());

    m.step(); // OUT (38H),A -- sets the pending flag, doesn't switch yet
    CHECK(m.sc7852Owns());
    CHECK(m.busArbiter().switchRequestedBySC7852());

    m.step(); // HALT -- SC7852 parks, arbiter completes the switch
    CHECK(!m.sc7852Owns());
    CHECK(m.sc7852().halted());
    CHECK(!m.busArbiter().switchRequestedBySC7852()); // consumed by the switch

    // Now LH5803 owns the bus -- its step() executes at its own reset PC.
    CHECK(m.lh5803().pc() == 0xC000);
    m.step();
    CHECK(!m.sc7852Owns()); // still LH5803's turn, no handoff requested yet
}

void test_lh5803_to_sc7852_handoff() {
    PC1600Machine m;
    std::vector<uint8_t> lower = makeBank(0x00);
    std::vector<uint8_t> upper = makeBank(0x00);
    lower[0] = 0xD3; lower[1] = 0x38; // OUT (38H),A
    lower[2] = 0x76;                  // HALT
    lower[3] = 0x00;                  // NOP -- what SC7852 resumes into after the round trip
    CHECK(m.loadBank0(lower.data(), lower.size(), upper.data(), upper.size()));

    // LH5803 ROM: STA #(0A038H) at its reset vector 0xC000 -- FD AE A0 38.
    std::vector<uint8_t> lh5803Rom(16384, 0x00);
    lh5803Rom[0x0000] = 0xFD; lh5803Rom[0x0001] = 0xAE;
    lh5803Rom[0x0002] = 0xA0; lh5803Rom[0x0003] = 0x38;
    lh5803Rom[16384 - 2] = 0xC0; // reset vector -> 0xC000
    lh5803Rom[16384 - 1] = 0x00;
    CHECK(m.loadLH5803Rom(lh5803Rom.data(), lh5803Rom.size()));

    m.reset();
    m.step(); // OUT (38H),A
    m.step(); // HALT -- switches to LH5803
    CHECK(!m.sc7852Owns());
    uint16_t sc7852ParkedPC = m.sc7852().pc(); // points at the NOP after HALT

    m.step(); // LH5803: STA #(0A038H) -- requests the switch back
    CHECK(m.sc7852Owns());
    CHECK(!m.sc7852().halted()); // parked with IFF1 clear: resumed directly (fallback)
    CHECK(m.sc7852().pc() == sc7852ParkedPC);

    m.step(); // SC7852 resumes normal execution: the NOP at its parked PC
    CHECK(m.sc7852Owns());
}

void test_trace_rings_are_independent_and_cpu_id_tagged() {
    PC1600Machine m;
    std::vector<uint8_t> lower = makeBank(0x00);
    std::vector<uint8_t> upper = makeBank(0x00);
    lower[0] = 0xD3; lower[1] = 0x38;
    lower[2] = 0x76;
    CHECK(m.loadBank0(lower.data(), lower.size(), upper.data(), upper.size()));
    std::vector<uint8_t> lh5803Rom(16384, 0x00);
    lh5803Rom[16384 - 2] = 0xC0;
    lh5803Rom[16384 - 1] = 0x00;
    CHECK(m.loadLH5803Rom(lh5803Rom.data(), lh5803Rom.size()));

    m.reset();
    m.sc7852().setTraceFlags(TRACE_PC);
    m.lh5803().setTraceFlags(TRACE_PC);

    m.step(); // OUT (38H),A on SC7852
    m.step(); // HALT on SC7852 -- switches to LH5803
    m.step(); // one LH5803 instruction

    Z80CpuFrame sc7852Frames[8];
    uint32_t lost = 0;
    uint32_t n = m.sc7852().drainTraceEvents(sc7852Frames, 8, &lost);
    CHECK(n == 2); // OUT + HALT, both recorded before the switch
    CHECK(sc7852Frames[0].cpuId == CPU_ID_SC7852);
    CHECK(sc7852Frames[1].cpuId == CPU_ID_SC7852);

    CpuFrame lh5803Frames[8];
    uint32_t n2 = m.lh5803().drainTraceEvents(lh5803Frames, 8, &lost);
    CHECK(n2 == 1); // just the one LH5803 instruction stepped above
    CHECK(lh5803Frames[0].cpuId == CPU_ID_LH5803);
}

// ── runCycles() pacing ────────────────────────────────────────────────
//
// These guard two pacing invariants: a halted step must credit its real
// cycle cost against the budget, not an arbitrary smaller value (otherwise
// idle time, and so the cursor blink, would run fast), and LH-5803 cycles
// must be scaled into the T-state budget by the clock ratio rather than
// summed raw (otherwise MODE 1 would pace at the wrong clock).

void test_runcycles_charges_halted_steps_at_the_halt_tick_rate() {
    PC1600Machine m;
    std::vector<uint8_t> lower = makeBank(0x00);
    std::vector<uint8_t> upper = makeBank(0x00);
    lower[0] = 0x76; // HALT at reset, with no interrupt enabled to wake it
    CHECK(m.loadBank0(lower.data(), lower.size(), upper.data(), upper.size()));
    m.reset();

    // A machine parked in HALT must still advance the budget -- otherwise
    // runCycles() would spin forever -- and at exactly the rate step()
    // credits its own timer accumulators, not some second opinion.
    m.step();
    CHECK(m.sc7852().halted());
    const uint64_t budget = 10000;
    const uint64_t consumed = m.runCycles(budget);
    CHECK(consumed >= budget);
    // Every step is halted, so the overshoot is bounded by one halt tick.
    CHECK(consumed < budget + SC7852::kHaltTickCycles);
    CHECK(consumed % SC7852::kHaltTickCycles == 0);
}

void test_yield_hook_fires_per_interval_across_runcycles_calls() {
    PC1600Machine m;
    std::vector<uint8_t> lower = makeBank(0x00);
    std::vector<uint8_t> upper = makeBank(0x00);
    lower[0] = 0x76; // HALT -- every step costs exactly one halt tick
    CHECK(m.loadBank0(lower.data(), lower.size(), upper.data(), upper.size()));
    m.reset();
    m.step();

    int calls = 0;
    const uint64_t interval = 100 * SC7852::kHaltTickCycles;
    m.setYieldHook([&calls] { calls++; }, interval);
    // Ten short calls add up to five intervals: the countdown must carry
    // over between runCycles() calls, not restart with each one.
    for (int i = 0; i < 10; ++i) m.runCycles(50 * SC7852::kHaltTickCycles);
    CHECK(calls == 5);

    m.setYieldHook({}, 0);
    m.runCycles(10 * interval);
    CHECK(calls == 5);
}

void test_half_second_tick_raises_srirq_bit1() {
    // The sub-CPU's 0.5 s tick raises SRIRQ (A2H) bit 1; step()'s 0.5 s
    // accumulator drives it, and the SRIRQ read clears it.
    PC1600Machine m;
    std::vector<uint8_t> lower = makeBank(0x00);
    std::vector<uint8_t> upper = makeBank(0x00);
    lower[0] = 0x76; // HALT -- halted steps still credit the accumulators
    CHECK(m.loadBank0(lower.data(), lower.size(), upper.data(), upper.size()));
    m.reset();

    auto& sub = m.memory().subCpu();
    sub.strobe(0xA2); (void)sub.readAnswer(); // start from nothing pending
    CHECK((sub.pendingInterrupts() & PC1600SubCpu::kIrqHalfSecond) == 0);
    // Just over one 0.5 s period of emulated time.
    m.runCycles(PC1600Machine::kTStateHz / 2 + PC1600Machine::kTStateHz / 20);
    CHECK((sub.pendingInterrupts() & PC1600SubCpu::kIrqHalfSecond) != 0);
    sub.strobe(0xA2);
    CHECK((sub.readAnswer() & PC1600SubCpu::kIrqHalfSecond) != 0);
    CHECK(sub.pendingInterrupts() == 0);
}

void test_runcycles_budget_is_tstates_in_either_bus_mode() {
    // The budget unit is SC-7852 T-states regardless of which CPU runs, so
    // one emulated second is kTStateHz in both modes. LH-5803 cycles are
    // scaled by the clock ratio rather than counted raw -- summing them raw
    // is what made MODE 1 pace at the wrong speed.
    CHECK(PC1600Machine::toTStates(1000, true) == 1000);
    const uint64_t scaled = PC1600Machine::toTStates(1000, false);
    CHECK(scaled == (1000ULL * PC1600Machine::kTStateHz +
                     PC1600Machine::kLH5803Hz / 2) / PC1600Machine::kLH5803Hz);
    CHECK(scaled > 2700 && scaled < 2760); // ~2.754x, the 3.58/1.3 ratio

    // The 1/64s timer's period derives from the same constant, so the two
    // can't disagree about how long an emulated second is.
    CHECK(PC1600Machine::kTStateHz == 3580000);
    CHECK(PC1600Machine::kLH5803Hz == 1300000);
}

void test_simple_reset_keeps_internal_ram_all_reset_wipes_it() {
    PC1600Machine m;
    // Internal RAM is page D bank 0, Z-80 window C000-FFFF.
    m.memory().write(0xC100, 0xAB);
    m.memory().write(0xF200, 0xCD);
    CHECK(m.memory().read(0xC100) == 0xAB);

    // Simple reset: the ROM's warm-start path depends on this surviving.
    m.reset();
    CHECK(m.memory().read(0xC100) == 0xAB);
    CHECK(m.memory().read(0xF200) == 0xCD);

    // ALL RESET: internal RAM gone -> ROM will run full cold init.
    m.allReset();
    CHECK(m.memory().read(0xC100) == 0x00);
    CHECK(m.memory().read(0xF200) == 0x00);
}

// A key whose host release never arrived (the GUI lost track of it) must
// not outlive a reset -- otherwise the machine stays wedged on it.
void test_reset_releases_held_keys() {
    PC1600Machine m;
    m.pressKey("*");
    m.pressKey("ctrl");
    CHECK(m.keyboard().scan(0x00, true) != 0xFF);
    m.reset();
    CHECK(m.keyboard().scan(0x00, true) == 0xFF);

    m.pressKey("*");
    m.allReset();
    CHECK(m.keyboard().scan(0x00, true) == 0xFF);
}

void test_reset_level_reported_to_boot_rom_via_request_5A() {
    PC1600Machine m;
    auto& bus = static_cast<SC7852Bus&>(m.memory());
    auto cause = [&] { bus.writeIO(0x21, 0x5A); return bus.readIO(0x33); };

    // Fresh machine: cold power-up -> ALL RESET (bit 5 set).
    CHECK((cause() & 0x20) != 0);

    m.reset();                       // simple reset -> bit 5 clear
    CHECK((cause() & 0x20) == 0);

    m.allReset();                    // ALL RESET -> bit 5 set again
    CHECK((cause() & 0x20) != 0);
}

// ── Debug reads (GUI debug panel: "Pointers" / "Dump Mem" buttons) ───────

void test_debug_peek_reads_internal_ram_through_current_banks() {
    PC1600Machine m;
    m.memory().write(0xF042, 0x9C); // PTR-table region, page D bank 0
    CHECK(m.debugPeek(0xF042) == 0x9C);
}

void test_debug_copy_internal_ram_is_the_live_state() {
    PC1600Machine m;
    m.memory().write(0xC000, 0xDD);
    m.memory().write(0xFFFF, 0xEE);

    std::vector<uint8_t> ram(PC1600Machine::kInternalRamSize);
    m.debugCopyInternalRam(ram.data());
    CHECK(ram.size() == 0x4000);
    CHECK(ram[0x0000] == 0xDD);
    CHECK(ram[0x3FFF] == 0xEE);
}

void test_debug_slot_image_returns_the_whole_card_backing_store() {
    PC1600Machine m;
    CHECK(m.debugSlotImage(1).empty()); // empty slot -> empty image
    CHECK(m.debugSlotImage(2).empty());

    // A banked card: 8 x 16 KB, contiguous, powered up 0x00.
    m.memory().attachSlot1Card(bundledCard("ce1638.card.yaml", CardHost::PC1600Slot1));
    std::vector<uint8_t> img = m.debugSlotImage(1);
    CHECK(img.size() == 8u * 0x4000);
    CHECK(img[0] == 0x00 && img.back() == 0x00);

    // Plain 32 KB RAM in Slot 2 -> a 32 KB image.
    m.memory().attachSlot2Card(plainRamCard(2 * PC1600Memory::kBankSize));
    CHECK(m.debugSlotImage(2).size() == 2u * 0x4000);
}

void test_debug_write_internal_ram_and_slot_image_land_directly() {
    PC1600Machine m;

    // Internal RAM: written bytes are visible through an ordinary peek.
    uint8_t block[3] = {0x11, 0x22, 0x33};
    CHECK(m.debugWriteInternalRam(0x00C5, block, 3));
    CHECK(m.debugPeek(0xC0C5) == 0x11);
    CHECK(m.debugPeek(0xC0C7) == 0x33);
    CHECK(!m.debugWriteInternalRam(0x3FFF, block, 3));   // runs past the 16 KB end
    CHECK(m.debugPeek(0xFFFF) == 0x00);                  // nothing written

    // Slot image: an empty slot rejects; an attached plain-RAM card takes
    // the write into its backing (visible via debugSlotImage()).
    CHECK(!m.debugWriteSlotImage(1, 0, block, 3));
    m.memory().attachSlot1Card(plainRamCard(2 * PC1600Memory::kBankSize));
    CHECK(m.debugWriteSlotImage(1, 0x4000, block, 3));   // start of the high 16 KB half
    auto img = m.debugSlotImage(1);
    CHECK(img.size() == 2u * 0x4000);
    CHECK(img[0x4000] == 0x11 && img[0x4002] == 0x33);
    CHECK(!m.debugWriteSlotImage(1, 0x7FFF, block, 3));  // past the 32 KB end -> nothing
    CHECK(m.debugSlotImage(1)[0x7FFF] == 0x00);
}

void test_debug_bank_state_reports_registers_and_card_bank() {
    PC1600Machine m;

    // Empty slots, all bank registers at reset: everything reads back 0.
    PC1600Machine::DebugBankState s = m.debugBankState();
    CHECK(s.port31 == 0 && s.port28 == 0 && s.port3c == 0);
    CHECK(s.pageABank == 0 && s.pageBBank == 0 && s.pageCBank == 0 && s.pageDBank == 0);
    CHECK(s.slot2MapMode == 0);
    CHECK(s.slot1MapRemapped == false);
    CHECK(s.slot1CardBank == -1); // no card -> no bank concept
    CHECK(s.slot2CardBank == -1);

    // Drive the registers the debug header surfaces.
    m.bank().writePort31(0x5A);  // page A bank 0, B 5, C 5, D 0  (0101'1010)
    m.bank().writePort28(0x03);  // Slot 2 vertical bank 3
    m.bank().writePort3C(0x24);  // SLOT1MAP b2 set, SLOT2MAP b5:4 = 10 -> mode 1

    // A banked card in Slot 1; its own latch starts at bank 0.
    m.memory().attachSlot1Card(bundledCard("ce1638.card.yaml", CardHost::PC1600Slot1));

    s = m.debugBankState();
    CHECK(s.port31 == 0x5A);
    CHECK(s.pageBBank == 5 && s.pageCBank == 5);
    CHECK(s.port28 == 0x03);
    CHECK(s.port3c == 0x24);
    CHECK(s.slot1MapRemapped == true);
    CHECK(s.slot2MapMode == 1);
    CHECK(s.slot1CardBank == 0); // card present, banked -> real latch value
    CHECK(s.slot2CardBank == -1);
}

void test_debug_bank_state_resolves_the_live_address_map() {
    using PT = PC1600Machine::PageTarget;
    PC1600Machine m;

    // Reset: A=sys ROM lo, B=sys ROM hi, C=Slot 1 (bank 0), D=internal RAM.
    PC1600Machine::DebugBankState s = m.debugBankState();
    CHECK(s.target[0] == PT::SystemRomLo);
    CHECK(s.target[1] == PT::SystemRomHi);
    CHECK(s.target[2] == PT::Slot1);
    CHECK(s.target[3] == PT::InternalRam);
    CHECK(!s.slotmapRedirect[0] && !s.slotmapRedirect[1] &&
          !s.slotmapRedirect[2] && !s.slotmapRedirect[3]);

    // Page C -> bank 6 (ROM IV), page B -> bank 3 hidden BASIC ROM.
    m.bank().writePort31(0x66);          // page B field (b3:1) = 3, page C field (b6:4) = 6
    m.bank().writePort3D(0x00);          // b2 clear -> hidden Bank 3b
    s = m.debugBankState();
    CHECK(s.target[1] == PT::Bank3bRom);
    CHECK(s.target[2] == PT::Bank6Rom);

    // SLOT2MAP mode 1 is armed but inert with no Slot 2 card and page C
    // not on bank 1.
    m.bank().writePort31(0x00);
    m.bank().writePort3C(0x20);          // b5 set -> mode 1
    s = m.debugBankState();
    CHECK(s.target[2] == PT::Slot1);
    CHECK(!s.slotmapRedirect[2]);

    // Now page C selects bank 1 AND a Slot 2 card is present: the mode-1
    // redirect goes live and steals page C for Slot 2.
    m.memory().attachSlot2Card(bundledCard("ce1638.card.yaml", CardHost::PC1600Slot2));
    m.bank().writePort31(0x10);          // page C field = 1
    s = m.debugBankState();
    CHECK(s.target[2] == PT::Slot2);
    CHECK(s.slotmapRedirect[2]);
}

// The ROM's OFF sequence in miniature: the SC-7852 hands the bus to the
// LH-5803 (OUT (38H) ; HALT), which sends the system-off command raw as
// 15H (= ~EAH) and halts (rom1500 E527H-E553H).
void loadOffSequence(PC1600Machine& m) {
    std::vector<uint8_t> lower = makeBank(0x00);
    std::vector<uint8_t> upper = makeBank(0x00);
    lower[0] = 0xD3; lower[1] = 0x38; // OUT (38H),A
    lower[2] = 0x76;                  // HALT
    CHECK(m.loadBank0(lower.data(), lower.size(), upper.data(), upper.size()));
    std::vector<uint8_t> lh5803Rom(16384, 0x00);
    const uint8_t off[] = {0xB5, 0x15,              // LDI A,15H
                           0xFD, 0xAE, 0x00, 0x21,  // STA #(0021H)
                           0xFD, 0xB1};             // HLT
    for (size_t i = 0; i < sizeof off; i++) lh5803Rom[i] = off[i];
    lh5803Rom[16384 - 2] = 0xC0;
    lh5803Rom[16384 - 1] = 0x00;
    CHECK(m.loadLH5803Rom(lh5803Rom.data(), lh5803Rom.size()));
    m.reset();
}

bool runUntilOff(PC1600Machine& m) {
    for (int i = 0; i < 1000 && !m.isPoweredOff(); i++) m.step();
    return m.isPoweredOff();
}

uint8_t subCpuPowerOnCause(PC1600Machine& m) {
    m.memory().subCpu().strobe(0xA5); // IOCS 15H
    return m.memory().subCpu().readAnswer();
}

// SubCpu doc §4: the system goes off once the sub-CPU has EAH and the CPU
// side halts; the calendar clock keeps running; the ON key's rising edge
// powers it on as a Z-80 reset that reports "ON key" (A5H bit 3).
void test_off_command_switches_power_and_on_key_resets() {
    PC1600Machine m;
    loadOffSequence(m);
    m.seedClock(2026, 9, 26, 10, 0, 0);
    CHECK(runUntilOff(m));
    CHECK(m.lh5803().halted());

    m.runCycles(static_cast<uint64_t>(PC1600Machine::kTStateHz) * 3);
    CHECK(m.isPoweredOff());
    CHECK(PC1600SubCpu::unpackBcd(m.memory().subCpu().dateTime().second) == 3);

    m.setOnKeyPressed(true);
    m.step();
    CHECK(!m.isPoweredOff());
    CHECK(m.sc7852Owns());
    CHECK(m.sc7852().pc() == 0x0000);     // came up through reset
    CHECK(subCpuPowerOnCause(m) == PC1600SubCpu::kCauseOnKey);
    // Held key: no fresh edge, nothing more happens.
    m.setOnKeyPressed(true);
    m.step();
    CHECK(!m.isPoweredOff());
}

// The wake-up timer matches either way, but only switches the system on
// when SWPON bit 1 allows it (WAKE$(0)).
void test_wake_timer_powers_on_only_when_enabled() {
    PC1600Machine m;
    loadOffSequence(m);
    auto& sub = m.memory().subCpu();
    auto setWake = [&](uint8_t minuteLo) {
        const uint8_t n[9] = {12, 2, 5, 0, 7, 3, minuteLo, 0, 0}; // 12/25 07:3x
        for (int i = 0; i < 9; i++) sub.strobe(static_cast<uint8_t>((i ? 0x80 : 0xF0) | n[i]));
        sub.strobe(0x94);                                         // SWWT
    };
    sub.setDateTime({12, 0x25, 0x07, 0x29, 0x58});
    setWake(0);
    CHECK(runUntilOff(m));
    m.runCycles(static_cast<uint64_t>(PC1600Machine::kTStateHz) * 3);
    CHECK(m.isPoweredOff());              // SWPON = 0: stays off...
    CHECK((sub.pendingInterrupts() & PC1600SubCpu::kIrqWakeUp) != 0); // ...but it matched

    sub.strobe(0xF2); sub.strobe(0xA4);   // SWPON = 2 (wake-up power-on)
    setWake(1);
    // Step until it comes on (this test program would switch it straight
    // back off), then check it came on at 07:31:00.
    for (uint64_t t = 0; m.isPoweredOff() && t < 62ull * PC1600Machine::kTStateHz;)
        t += static_cast<uint64_t>(m.step());
    CHECK(!m.isPoweredOff());
    CHECK(sub.dateTime().minute == 0x31 && sub.dateTime().second == 0x00);
    CHECK(subCpuPowerOnCause(m) == PC1600SubCpu::kCauseWakeUp);
}

// RS-232C CI switches the system on when SWPON bit 0 allows it (WAKE$(1)).
void test_ci_powers_on_when_enabled() {
    PC1600Machine m;
    loadOffSequence(m);
    auto& sub = m.memory().subCpu();
    CHECK(runUntilOff(m));
    sub.setCiLine(true);
    m.step();
    CHECK(m.isPoweredOff());              // SWPON = 0
    sub.setCiLine(false);
    sub.strobe(0xF1); sub.strobe(0xA4);   // SWPON = 1
    sub.setCiLine(true);
    m.step();
    CHECK(!m.isPoweredOff());
    CHECK(subCpuPowerOnCause(m) == PC1600SubCpu::kCauseCi);
}

// The ROM's handoff (P1-B3 5C0E-5C22): 35H = 08H, OUT (38H), EI, HALT.
// The LH5803's STA #(0A038H) then raises cause bit 3, and that INT -- not a
// direct resume -- ends the SC7852's HALT.
void test_lh5803_handback_is_a_cause_bit3_interrupt() {
    PC1600Machine m;
    std::vector<uint8_t> lower = makeBank(0x00);
    std::vector<uint8_t> upper = makeBank(0x00);
    const uint8_t park[] = {0xED, 0x56,        // IM 1 (vector 0038H)
                            0x3E, 0x08, 0xD3, 0x35, // 35H = 08H
                            0xD3, 0x38,        // OUT (38H),A
                            0xFB, 0x76, 0x00}; // EI ; HALT ; NOP
    for (size_t i = 0; i < sizeof park; i++) lower[i] = park[i];
    CHECK(m.loadBank0(lower.data(), lower.size(), upper.data(), upper.size()));
    std::vector<uint8_t> lh5803Rom(16384, 0x00);
    lh5803Rom[0] = 0xFD; lh5803Rom[1] = 0xAE; lh5803Rom[2] = 0xA0; lh5803Rom[3] = 0x38; // STA #(0A038H)
    lh5803Rom[16384 - 2] = 0xC0;
    lh5803Rom[16384 - 1] = 0x00;
    CHECK(m.loadLH5803Rom(lh5803Rom.data(), lh5803Rom.size()));
    m.reset();

    for (int i = 0; i < 6; i++) m.step(); // IM 1, LD, OUT (35H), OUT (38H), EI, HALT
    CHECK(!m.sc7852Owns());
    m.step();                             // LH5803: STA #(0A038H)
    CHECK(m.sc7852Owns());
    CHECK(m.sc7852().halted());           // still parked: the INT ends it, not the switch
    CHECK(m.sc7852().intLine());
    m.step();                             // INT accepted
    CHECK(!m.sc7852().halted());
    CHECK(m.sc7852().pc() == 0x0038);
    CHECK(!m.sc7852().iff1());
    CHECK((m.memory().readIO(0x32) & 0x08) == 0x08);
}

// An ON press while the bus owner is running is a BREAK the ROM reads from
// the 1BH latch -- it must not stay pending and later wake the machine out
// of the next power-down park.
void test_on_press_while_running_does_not_wake_a_later_park() {
    PC1600Machine m;
    std::vector<uint8_t> lower = makeBank(0x00);
    std::vector<uint8_t> upper = makeBank(0x00);
    lower[0] = 0x00;                  // NOP -- "running"
    lower[1] = 0xD3; lower[2] = 0x38; // OUT (38H),A
    lower[3] = 0x76;                  // HALT
    CHECK(m.loadBank0(lower.data(), lower.size(), upper.data(), upper.size()));
    std::vector<uint8_t> lh5803Rom(16384, 0x00);
    lh5803Rom[0] = 0xFD; lh5803Rom[1] = 0xB1; // HLT
    lh5803Rom[16384 - 2] = 0xC0;
    lh5803Rom[16384 - 1] = 0x00;
    CHECK(m.loadLH5803Rom(lh5803Rom.data(), lh5803Rom.size()));
    m.reset();

    m.setOnKeyPressed(true);    // BREAK while the SC7852 runs
    m.setOnKeyPressed(false);
    m.step();                   // NOP
    m.step();                   // OUT (38H),A
    m.step();                   // HALT -> bus to the LH5803
    m.step();                   // LH5803: HLT
    CHECK(m.lh5803().halted());
    m.runCycles(5000);
    CHECK(m.lh5803().halted()); // still parked
    CHECK(!m.isPoweredOff());   // a HALT alone is not a power-off
}


void test_rtc_advances_while_lh5803_owns_the_bus() {
    // The OFF-key / auto-power-off shutdown hands the bus to the LH5803 for
    // the whole powered-off span, and the calendar clock must keep
    // advancing even though the SC7852 isn't the one stepping -- it lives
    // on the always-powered VGG rail on real hardware.
    PC1600Machine m;
    std::vector<uint8_t> lower = makeBank(0x00);
    std::vector<uint8_t> upper = makeBank(0x00);
    lower[0] = 0xD3; lower[1] = 0x38; // OUT (38H),A
    lower[2] = 0x76;                  // HALT -- completes the handoff to LH5803
    CHECK(m.loadBank0(lower.data(), lower.size(), upper.data(), upper.size()));

    // LH5803 ROM: HLT at its own reset vector, same "parked off" shape as
    // test_on_key_wakes_a_halted_lh5803_owning_the_bus() above.
    std::vector<uint8_t> lh5803Rom(16384, 0x00);
    lh5803Rom[0x0000] = 0xFD; lh5803Rom[0x0001] = 0xB1;
    lh5803Rom[16384 - 2] = 0xC0;
    lh5803Rom[16384 - 1] = 0x00;
    CHECK(m.loadLH5803Rom(lh5803Rom.data(), lh5803Rom.size()));

    m.reset();
    m.seedClock(2026, 1, 1, 23, 59, 58); // 2 s before a minute/hour/day rollover
    m.step(); // OUT (38H),A
    m.step(); // HALT -- switches bus ownership to LH5803
    m.step(); // LH5803 executes its own HLT at 0xC000
    CHECK(!m.sc7852Owns());
    CHECK(m.lh5803().halted());

    // Three emulated seconds' worth of T-states, entirely while the LH5803
    // owns the bus and stays halted -- the SC7852 never steps again.
    // kRtcPeriodTStates (private) == kTStateHz, one emulated second.
    m.runCycles(static_cast<uint64_t>(PC1600Machine::kTStateHz) * 3);
    CHECK(!m.sc7852Owns());
    CHECK(m.lh5803().halted());

    const PC1600SubCpu::DateTime dt = m.memory().subCpu().dateTime();
    CHECK(PC1600SubCpu::unpackBcd(dt.second) == 1); // 58 -> 59 -> 0 -> 1, carrying minute/hour/day
    CHECK(PC1600SubCpu::unpackBcd(dt.minute) == 0);
    CHECK(PC1600SubCpu::unpackBcd(dt.hour) == 0);
    CHECK(PC1600SubCpu::unpackBcd(dt.day) == 2);
}

// The chip only takes whole seconds, so seedClock()'s millisecond preloads
// the 1 Hz accumulator: seeded at hh:mm:ss.900, the next tick must come
// 0.1 s later, not a full second (which left the clock ~1 s behind after
// every reset). Seeded at .000, it takes the full second.
void test_seed_clock_millisecond_aligns_next_tick() {
    PC1600Machine m;
    std::vector<uint8_t> lower = makeBank(0x00);
    std::vector<uint8_t> upper = makeBank(0x00);
    lower[0] = 0x76; // HALT -- parked, just burns T-states
    CHECK(m.loadBank0(lower.data(), lower.size(), upper.data(), upper.size()));
    const auto tenth = static_cast<uint64_t>(PC1600Machine::kTStateHz) / 10;

    m.reset();
    m.seedClock(2026, 9, 23, 12, 0, 10, 900);
    m.runCycles(tenth * 2);
    CHECK(PC1600SubCpu::unpackBcd(m.memory().subCpu().dateTime().second) == 11);

    m.reset();
    m.seedClock(2026, 9, 23, 12, 0, 10, 0);
    m.runCycles(tenth * 2);
    CHECK(PC1600SubCpu::unpackBcd(m.memory().subCpu().dateTime().second) == 10);
    m.runCycles(tenth * 9);
    CHECK(PC1600SubCpu::unpackBcd(m.memory().subCpu().dateTime().second) == 11);
}

} // namespace

// INT is the level (cause & mask): masking a latched cause at 35H or the
// 32H read that clears it withdraws the request.
void test_int_line_follows_cause_and_mask() {
    PC1600Machine m;
    auto& mem = m.memory();
    CHECK(!m.sc7852().intLine());
    mem.writeIO(0x35, 0x10);
    mem.latchTimer64InterruptCause();
    CHECK(m.sc7852().intLine());
    mem.writeIO(0x35, 0x00);
    CHECK(!m.sc7852().intLine()); // masked: withdrawn
    mem.writeIO(0x35, 0x10);
    CHECK(m.sc7852().intLine());  // still latched, re-enabled
    mem.writeIO(0x23, 0xC5);                    // pr[5] = TxINTM: keep the UART's bit 0 out
    mem.writeIO(0x22, 0x02);
    CHECK(mem.readIO(0x32) == 0x10);
    CHECK(!m.sc7852().intLine()); // read-clear drops it

    // A cause arriving while masked at 35H is still there; unmasking raises
    // INT. Bit 6 is the sub-CPU's Z7, a level its own mask (SWMSK) gates.
    mem.writeIO(0x35, 0x00);
    mem.subCpu().strobe(0xF0); mem.subCpu().strobe(0x82); mem.subCpu().strobe(0xA0); // SWMSK 02H
    mem.subCpu().halfSecondTick();
    CHECK(!m.sc7852().intLine());
    mem.writeIO(0x35, 0x40);
    CHECK(m.sc7852().intLine());
    mem.subCpu().strobe(0xA2);                  // SRIRQ read drops Z7
    CHECK(!m.sc7852().intLine());

    // Bit 0 is the TC8576F's live INT output, not a latch: a 32H read
    // leaves it (and INT) up until the chip itself is serviced.
    // The transmit interrupt needs a peer's CTS and TxEN (CPC §6.5).
    mem.readIO(0x32);
    struct CtsLink : SerialLink {
        bool poll(uint8_t&) override { return false; }
        void send(uint8_t) override {}
    } link;
    m.setSerialLink(&link);
    mem.uart().tick(1);                         // pick up CTS
    mem.writeIO(0x23, 0x01);                    // TxEN
    mem.writeIO(0x23, 0xC5);                    // pr[5] = 0: TxINTM clear
    mem.writeIO(0x22, 0x00);
    mem.writeIO(0x35, 0x01);
    CHECK(m.memory().uart().interruptOutput()); // TxRDY with TxINTM clear
    CHECK(m.sc7852().intLine());
    CHECK((mem.readIO(0x32) & 0x01) == 0x01);
    CHECK(m.sc7852().intLine());
    mem.writeIO(0x23, 0xC5);                    // pr[5] = TxINTM
    mem.writeIO(0x22, 0x02);
    CHECK(!m.sc7852().intLine());
    CHECK((mem.readIO(0x32) & 0x01) == 0x00);
    m.setSerialLink(nullptr);

    // Reset clears cause and mask, and the line with them.
    m.reset();
    CHECK(!m.sc7852().intLine());
    CHECK(mem.readIO(0x32) == 0x00);
    CHECK(mem.intMask() == 0x00);
}

static int litPixels(const PC1600Machine& m) {
    const PC1600DisplaySnapshot snap = m.displaySnapshot();
    int n = 0;
    for (const auto& row : snap.pixels)
        for (bool px : row) n += px ? 1 : 0;
    return n;
}

// ROM-gated: OFF parks the machine, it stays parked (no stray interrupt
// brings it back), and ON restarts it -- several times over.
void test_real_rom_off_stays_off_and_on_restarts() {
    PC1600Machine m;
    if (!bootPC1600(m)) {
        std::fprintf(stderr, "SKIP test_real_rom_off_stays_off_and_on_restarts: PC-1600 ROM images not found\n");
        return;
    }
    const uint64_t hz = static_cast<uint64_t>(PC1600Machine::kTStateHz);
    CHECK(litPixels(m) > 0);
    for (int cycle = 0; cycle < 3; cycle++) {
        m.pressKey("off");
        m.runCycles(hz / 2);
        m.releaseKey("off");
        m.runCycles(hz * 3);
        CHECK(litPixels(m) == 0);
        m.runCycles(hz * 5); // long enough for many 64 Hz / 0.5 s edges
        CHECK(litPixels(m) == 0);
        CHECK(!m.sc7852Owns() || m.sc7852().halted());

        m.setOnKeyPressed(true);
        m.runCycles(hz / 4);
        m.setOnKeyPressed(false);
        m.runCycles(hz * 3);
        CHECK(litPixels(m) > 0);
        CHECK(m.sc7852Owns());
    }
}

// Real ROM: ON TIME$ GOSUB fires. BASIC stores the time with SWA1T (96H);
// at the matching minute carry the sub-CPU raises SRIRQ bit 6, the INT6
// handler sets F127H bit 6, and the interpreter branches (SubCpu §5).
void typeProgram(PC1600Machine& m, std::initializer_list<const char*> lines) {
    std::string err;
    for (const char* l : lines) CHECK(typeLine(m, l, /*pressEnter=*/true, &err));
}

void test_rom_on_time_gosub_fires() {
    PC1600Machine m;
    if (!bootPC1600(m)) {
        std::fprintf(stderr, "SKIP test_rom_on_time_gosub_fires: PC-1600 ROM images not found\n");
        return;
    }
    tapKey(m, "mode"); // RUN -> PRO
    waitIdle(m, PC1600Machine::kTStateHz);
    typeProgram(m, {
        "10 POKE &FF80,0",
        "20 DATE$=\"09/26\":TIME$=\"13:29:57\"",
        "30 ON TIME$=\"09/26/13/30\" GOSUB 100",
        "40 TIME$ ON",
        "50 GOTO 50",
        "100 POKE &FF80,123",
        "110 END",
    });
    tapKey(m, "mode"); // PRO -> RUN
    waitIdle(m, PC1600Machine::kTStateHz);
    std::string err;
    typeLine(m, "RUN", /*pressEnter=*/true, &err);
    // Step in 50 ms slices until the handler has run; it must be at the
    // 13:30:00 minute carry, not before.
    PC1600SubCpu::DateTime at{};
    for (int i = 0; i < 200 && m.memory().read(0xFF80) != 123; i++) {
        at = m.memory().subCpu().dateTime();
        m.runCycles(PC1600Machine::kTStateHz / 20);
    }
    CHECK(m.memory().read(0xFF80) == 123);
    CHECK(at.hour == 0x13 && at.minute == 0x30 && at.second == 0x00);
}

bool runUntil(PC1600Machine& m, bool wantOff, double seconds) {
    const uint64_t limit = static_cast<uint64_t>(seconds * PC1600Machine::kTStateHz);
    for (uint64_t t = 0; m.isPoweredOff() != wantOff && t < limit;) t += m.runCycles(PC1600Machine::kTStateHz / 100);
    return m.isPoweredOff() == wantOff;
}

// Real ROM: the OFF key runs the ROM's power-off (signature at FA08H, EAH
// from the LH-5803), the system goes off, and ON brings it back through a
// reset that resumes -- program kept, prompt responsive (SubCpu §4.1).
void test_rom_off_on_resumes() {
    PC1600Machine m;
    if (!bootPC1600(m)) {
        std::fprintf(stderr, "SKIP test_rom_off_on_resumes: PC-1600 ROM images not found\n");
        return;
    }
    tapKey(m, "mode"); // RUN -> PRO
    waitIdle(m, PC1600Machine::kTStateHz);
    typeProgram(m, {"10 PRINT 1"});
    const uint16_t end = static_cast<uint16_t>((m.memory().read(0xF867) << 8) | m.memory().read(0xF868));
    tapKey(m, "mode"); // PRO -> RUN
    waitIdle(m, PC1600Machine::kTStateHz);

    tapKey(m, "off");
    CHECK(runUntil(m, /*wantOff=*/true, 3.0));
    // The OFF key's power-off (P0-B0 0B07H) marks FA08H with AAH x 4.
    for (int i = 0; i < 4; i++) CHECK(m.memory().read(static_cast<uint16_t>(0xFA08 + i)) == 0xAA);
    m.runCycles(static_cast<uint64_t>(PC1600Machine::kTStateHz) * 2);
    CHECK(m.isPoweredOff());

    m.setOnKeyPressed(true);
    m.runCycles(PC1600Machine::kTStateHz / 10);
    m.setOnKeyPressed(false);
    CHECK(!m.isPoweredOff());
    // The boot waits for ON's release, then comes back to the prompt.
    for (int k = 0; k < 30 && !(m.sc7852().halted() && m.sc7852().pc() == 0x92B3); k++)
        m.runCycles(PC1600Machine::kTStateHz / 10);
    waitIdle(m, static_cast<uint64_t>(PC1600Machine::kTStateHz) * 2);
    const uint16_t endAfter = static_cast<uint16_t>((m.memory().read(0xF867) << 8) | m.memory().read(0xF868));
    CHECK(endAfter == end);
    std::string err;
    CHECK(typeLine(m, "POKE &FF80,55", /*pressEnter=*/true, &err));
    waitIdle(m, PC1600Machine::kTStateHz);
    CHECK(m.memory().read(0xFF80) == 55);
}

// Real ROM: WAKE$(0) switches a POWER OFF machine on at the set minute and
// runs its command string (FA1BH bit 6 -> FF00H to the key buffer). The
// ROM separates the command with ':' (romIII-3 6E23H compares 3AH).
void test_rom_wake_runs_command_at_the_set_time() {
    PC1600Machine m;
    if (!bootPC1600(m)) {
        std::fprintf(stderr, "SKIP test_rom_wake_runs_command_at_the_set_time: PC-1600 ROM images not found\n");
        return;
    }
    tapKey(m, "mode"); // RUN -> PRO -> RUN, as after any ALL RESET here
    waitIdle(m, PC1600Machine::kTStateHz);
    tapKey(m, "mode");
    waitIdle(m, PC1600Machine::kTStateHz);
    std::string err;
    for (const char* l : {"TIME$=\"13:30:00\"", "DATE$=\"09/26\"", "POKE &FF80,0",
                          "WAKE$(0)=\"09/26/13/31:POKE &FF80,77\"+CHR$(13)"}) {
        CHECK(typeLine(m, l, /*pressEnter=*/true, &err));
        waitIdle(m, PC1600Machine::kTStateHz);
    }
    const PC1600SubCpu::Alarm w = m.memory().subCpu().timer(PC1600SubCpu::WakeUp);
    CHECK(w.month == 9 && w.day == 0x26 && w.hour == 0x13 && w.minute == 0x31);
    CHECK(typeLine(m, "POWER OFF", /*pressEnter=*/true, &err));
    CHECK(runUntil(m, /*wantOff=*/true, 3.0));
    CHECK(runUntil(m, /*wantOff=*/false, 70.0));
    const PC1600SubCpu::DateTime at = m.memory().subCpu().dateTime();
    CHECK(at.minute == 0x31 && at.second == 0x00);
    m.runCycles(static_cast<uint64_t>(PC1600Machine::kTStateHz) * 5);
    CHECK(m.memory().read(0xFF80) == 77);
}

// Real ROM: auto power-off after 10 idle minutes (the INT6 handler's
// countdown, P1-B3 426AH -> 0005H -> P0-B0 0B66H) saves SP at F0DAH and
// marks FA08H with A5H x 4. ON then resumes through that signature, which
// the resume path clears (P0-B0 07E6H/07FEH).
void test_rom_auto_power_off_resumes() {
    PC1600Machine m;
    if (!bootPC1600(m)) {
        std::fprintf(stderr, "SKIP test_rom_auto_power_off_resumes: PC-1600 ROM images not found\n");
        return;
    }
    tapKey(m, "mode"); waitIdle(m, PC1600Machine::kTStateHz);
    tapKey(m, "mode"); waitIdle(m, PC1600Machine::kTStateHz);
    CHECK(runUntil(m, /*wantOff=*/true, 700.0));
    CHECK(m.memory().subCpu().dateTime().minute == 0x10);
    for (int i = 0; i < 4; i++) CHECK(m.memory().read(static_cast<uint16_t>(0xFA08 + i)) == 0xA5);

    m.setOnKeyPressed(true);
    m.runCycles(PC1600Machine::kTStateHz / 10);
    m.setOnKeyPressed(false);
    for (int k = 0; k < 30 && !(m.sc7852().halted() && m.sc7852().pc() == 0x92B3); k++)
        m.runCycles(PC1600Machine::kTStateHz / 10);
    CHECK(!m.isPoweredOff());
    CHECK(m.memory().read(0xFA1B) == 0x10);  // power-on by the ON key
    CHECK(m.memory().read(0xFA08) == 0x00);  // signature consumed: resumed
    std::string err;
    CHECK(typeLine(m, "POKE &FF80,55", /*pressEnter=*/true, &err));
    waitIdle(m, PC1600Machine::kTStateHz);
    CHECK(m.memory().read(0xFF80) == 55);
}

// Real ROM: a COM1: transfer through the TC8576F to an attached peer --
// SETCOM, OPEN, PRINT#, CLOSE. Exercises the CPC as the ROM programs it:
// PR7/PR1:PR0 baud, the serial command shadow (TxEN), and the CS/CD/DR
// status polarity the ROM checks before it sends (PC-1600-CPC-TC8576.md §9).
struct RecordingLink : SerialLink {
    std::vector<uint8_t> tx;
    std::deque<uint8_t> rx;
    uint32_t baud = 0;
    bool poll(uint8_t& out) override {
        if (rx.empty()) return false;
        out = rx.front(); rx.pop_front(); return true;
    }
    void send(uint8_t b) override { tx.push_back(b); }
    void onBaud(uint32_t b, int) override { baud = b; }
    Lines lines;
    void getStatus(Lines& in) override { in = lines; }
};

void test_rom_com1_print_reaches_the_peer() {
    PC1600Machine m;
    if (!bootPC1600(m)) {
        std::fprintf(stderr, "SKIP test_rom_com1_print_reaches_the_peer: PC-1600 ROM images not found\n");
        return;
    }
    RecordingLink link;
    m.setSerialLink(&link);
    tapKey(m, "mode"); waitIdle(m, PC1600Machine::kTStateHz);
    tapKey(m, "mode"); waitIdle(m, PC1600Machine::kTStateHz);
    std::string err;
    auto type = [&](const char* l) {
        CHECK(typeLine(m, l, /*pressEnter=*/true, &err));
        waitIdle(m, PC1600Machine::kTStateHz);
    };
    // SNDSTAT 24: send only while CS is on, 1 s timeout.
    for (const char* l : {"MAXFILES=1", "SETCOM \"COM1:\",9600,8,N,1,N,N", "SNDSTAT \"COM1:\",24,2",
                          "OPEN \"COM1:\" FOR OUTPUT AS #1", "PRINT #1,\"HELLO\"", "CLOSE #1"})
        type(l);
    m.runCycles(PC1600Machine::kTStateHz);
    CHECK(link.baud == 9600);
    CHECK(std::string(link.tx.begin(), link.tx.end()).find("HELLO") != std::string::npos);

    // CS off: the ROM holds the data back (P2-B6 A524H gating) and times out.
    link.lines.cts = false;
    link.tx.clear();
    for (const char* l : {"OPEN \"COM1:\" FOR OUTPUT AS #1", "PRINT #1,\"WORLD\"", "CLOSE #1"}) type(l);
    m.runCycles(static_cast<uint64_t>(PC1600Machine::kTStateHz) * 2);
    CHECK(std::string(link.tx.begin(), link.tx.end()).find("WORLD") == std::string::npos);
    m.setSerialLink(nullptr);
}

int run_pc1600_machine_tests() {
    test_rom_com1_print_reaches_the_peer();
    test_rom_auto_power_off_resumes();
    test_rom_off_on_resumes();
    test_rom_wake_runs_command_at_the_set_time();
    test_off_command_switches_power_and_on_key_resets();
    test_wake_timer_powers_on_only_when_enabled();
    test_ci_powers_on_when_enabled();
    test_rom_on_time_gosub_fires();
    test_simple_reset_keeps_internal_ram_all_reset_wipes_it();
    test_reset_releases_held_keys();
    test_reset_level_reported_to_boot_rom_via_request_5A();
    test_debug_peek_reads_internal_ram_through_current_banks();
    test_debug_copy_internal_ram_is_the_live_state();
    test_debug_slot_image_returns_the_whole_card_backing_store();
    test_debug_write_internal_ram_and_slot_image_land_directly();
    test_debug_bank_state_reports_registers_and_card_bank();
    test_debug_bank_state_resolves_the_live_address_map();
    test_reset_starts_on_sc7852();
    test_sc7852_to_lh5803_handoff();
    test_lh5803_to_sc7852_handoff();
    test_trace_rings_are_independent_and_cpu_id_tagged();
    test_runcycles_charges_halted_steps_at_the_halt_tick_rate();
    test_yield_hook_fires_per_interval_across_runcycles_calls();
    test_half_second_tick_raises_srirq_bit1();
    test_runcycles_budget_is_tstates_in_either_bus_mode();
    test_int_line_follows_cause_and_mask();
    test_lh5803_handback_is_a_cause_bit3_interrupt();
    test_on_press_while_running_does_not_wake_a_later_park();
    test_real_rom_off_stays_off_and_on_restarts();
    test_rtc_advances_while_lh5803_owns_the_bus();
    test_seed_clock_millisecond_aligns_next_tick();

    std::printf("pc1600_machine_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
