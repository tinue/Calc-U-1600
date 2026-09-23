// Headless C++ tests for the dual-CPU facade
// (Core/PC1600/PC1600Machine.hpp/.cpp, PC1600BusArbiter.hpp). Same
// no-framework, assert-and-tally style as lh5801_tests.cpp -- see that
// file's header comment.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <vector>

#include "../PC1600/PC1600Machine.hpp"
#include "TestCards.hpp"

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
    CHECK(!m.sc7852().halted()); // resumed via resumeFromHalt(), not a real interrupt
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

void test_half_second_signal_toggles_off_the_05s_accumulator() {
    // The sub-CPU's 0.5 s signal (bit 1 of request 5DH) must actually
    // toggle for the file/RAM-disk IOCS readiness handshake to progress --
    // step()'s 0.5 s accumulator drives it.
    PC1600Machine m;
    std::vector<uint8_t> lower = makeBank(0x00);
    std::vector<uint8_t> upper = makeBank(0x00);
    lower[0] = 0x76; // HALT -- halted steps still credit the accumulators
    CHECK(m.loadBank0(lower.data(), lower.size(), upper.data(), upper.size()));
    m.reset();

    const bool start = m.memory().subCpu().halfSecondSignal();
    // Just over one 0.5 s period of emulated time.
    m.runCycles(PC1600Machine::kTStateHz / 2 + PC1600Machine::kTStateHz / 20);
    CHECK(m.memory().subCpu().halfSecondSignal() != start);
    // A second period brings it back.
    m.runCycles(PC1600Machine::kTStateHz / 2 + PC1600Machine::kTStateHz / 20);
    CHECK(m.memory().subCpu().halfSecondSignal() == start);
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

// The ON/BREAK key must wake the SC7852 from a HALT that has no periodic
// interrupt to end it -- the state the ROM's auto-power-off / OFF-key
// power-down leaves it in (every port-35H cause masked, HALT with IFF1=1).
// Only the 64 Hz / 0.5 s timers otherwise raise interrupts, both masked in
// this state, so the ON key must be its own independent wake source.
void test_on_key_wakes_a_halted_sc7852() {
    PC1600Machine m;
    std::vector<uint8_t> lower = makeBank(0x00);
    std::vector<uint8_t> upper = makeBank(0x00);
    lower[0] = 0x76; // HALT at reset -- stands in for the power-down park
    CHECK(m.loadBank0(lower.data(), lower.size(), upper.data(), upper.size()));
    m.reset();

    m.step();
    CHECK(m.sc7852().halted());

    // No periodic interrupt is unmasked at reset, so it stays halted no
    // matter how long runCycles() spins.
    m.runCycles(5000);
    CHECK(m.sc7852().halted());

    // Pressing ON raises the wake interrupt; the next step resumes it.
    m.setOnKeyPressed(true);
    m.step();
    CHECK(!m.sc7852().halted());

    // A second press with the key still held is not a fresh edge -- no
    // extra interrupt (the CPU is already running; nothing to assert
    // beyond "doesn't crash / re-halt").
    m.setOnKeyPressed(true);
    m.step();
    CHECK(!m.sc7852().halted());
}

// Same wake requirement, but with the bus already handed to the LH5803:
// the SC7852 issues its documented `OUT (38H),A` handoff before its own
// power-down HALT, so the machine can be frozen with the LH5803 parked
// instead of the SC7852. Waking only the SC7852 is not enough; the LH5803
// is LH5801-family, whose ordinary requestMaskableInterrupt() is IE-gated
// and does nothing to a HALT with IE clear (the reset-time default) --
// wakeFromHalt() is the unconditional counterpart this needs.
void test_on_key_wakes_a_halted_lh5803_owning_the_bus() {
    PC1600Machine m;
    std::vector<uint8_t> lower = makeBank(0x00);
    std::vector<uint8_t> upper = makeBank(0x00);
    lower[0] = 0xD3; lower[1] = 0x38; // OUT (38H),A
    lower[2] = 0x76;                  // HALT -- completes the handoff to LH5803
    CHECK(m.loadBank0(lower.data(), lower.size(), upper.data(), upper.size()));

    // LH5803 ROM: HLT (0xFD 0xB1 -- the 0xFD-prefixed special-op encoding,
    // matching lh5801_tests.cpp's own Rig) at its own reset vector
    // (0xC000). IE is clear at reset, so this HLT cannot be woken by an
    // ordinary maskable interrupt -- exactly the state a real power-down
    // HALT is in.
    std::vector<uint8_t> lh5803Rom(16384, 0x00);
    lh5803Rom[0x0000] = 0xFD; lh5803Rom[0x0001] = 0xB1;
    lh5803Rom[16384 - 2] = 0xC0;
    lh5803Rom[16384 - 1] = 0x00;
    CHECK(m.loadLH5803Rom(lh5803Rom.data(), lh5803Rom.size()));

    m.reset();
    m.step(); // OUT (38H),A
    m.step(); // HALT -- switches bus ownership to LH5803
    CHECK(!m.sc7852Owns());

    m.step(); // LH5803 executes its own HLT at 0xC000
    CHECK(m.lh5803().halted());

    // No periodic interrupt reaches an LH5803 HLT, so it stays halted no
    // matter how long runCycles() spins.
    m.runCycles(5000);
    CHECK(m.lh5803().halted());

    // Pressing ON wakes it even though bus ownership never returned to the
    // SC7852 and IE was never set.
    m.setOnKeyPressed(true);
    m.step();
    CHECK(!m.lh5803().halted());
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

int run_pc1600_machine_tests() {
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
    test_half_second_signal_toggles_off_the_05s_accumulator();
    test_runcycles_budget_is_tstates_in_either_bus_mode();
    test_on_key_wakes_a_halted_sc7852();
    test_on_key_wakes_a_halted_lh5803_owning_the_bus();
    test_rtc_advances_while_lh5803_owns_the_bus();
    test_seed_clock_millisecond_aligns_next_tick();

    std::printf("pc1600_machine_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
