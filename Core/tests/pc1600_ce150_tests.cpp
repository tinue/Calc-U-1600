// Phase 2: the PC-1500's CE-150 plotter attached to the PC-1600's LH5803
// (compatibility) side. Verifies the wiring in LH5803SharedMemory +
// PC1600Machine -- the CE-150 mechanism itself is already covered
// end-to-end on the PC-1500 in ce150_tests.cpp, so this focuses on the
// LH5803-side ROM window (PV-gated), the ME1 0xB008-0xB00F block, motor
// writes reaching the shared mechanism, and CE-150 / CE-1600P mutual
// exclusion on the bus.
//
// Build & run: see tools/run_tests.sh

#include <cstdio>
#include <cstdint>
#include <vector>

#include "../Connector/Ce150Card.hpp"
#include "../PC1600/PC1600Machine.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

constexpr uint8_t kRing[8] = {0x9, 0x8, 0xc, 0x4, 0x6, 0x2, 0x3, 0x1};

std::vector<uint8_t> fakeRom(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = uint8_t(i * 5 + seed);
    return v;
}

void test_lh5803_rom_window_ignores_cpu_pv() {
    PC1600Machine m;
    auto rom = fakeRom(Ce150Card::kRomSize, 1);
    CHECK(m.attachCE150(rom.data(), rom.size()));
    CHECK(m.ce150Attached());

    auto& mem = m.lh5803Memory();
    // The CE-150 ROM shows in the LH5803's 0xA000-0xBFFF window whenever a
    // card is attached. LH5803SharedMemory presents PV=0 to the card on
    // this path unconditionally -- NOT the CPU's PV flip-flop -- because
    // the CE-150 lives at PVOUT=0 and our LH5803 core does not yet model
    // the CALLH/PARBAN bank bridge that would make m_pv trustworthy here
    // (see the comment in LH5803SharedMemory::readME0). So the same bytes
    // come back at CPU PV = 0 and CPU PV = 1.
    for (bool pv : {false, true}) {
        mem.updatePUPV(false, pv);
        CHECK(mem.readME0(0xA000) == rom[0]);
        CHECK(mem.readME0(0xBFFF) == rom[0x1FFF]);
    }
    // Below the window is open bus regardless.
    CHECK(mem.readME0(0x9FFF) == 0xFF);

    m.detachCE150();
    CHECK(!m.ce150Attached());
    CHECK(mem.readME0(0xA000) == 0xFF);
}

void test_lh5803_me1_block_and_motor_writes() {
    PC1600Machine m;
    auto rom = fakeRom(Ce150Card::kRomSize, 2);
    m.attachCE150(rom.data(), rom.size());
    auto& mem = m.lh5803Memory();

    // LH5810 DDA latch round-trips through the card at LH5803 ME1 0xB00C
    // (not PV-gated -- register access, unlike the ROM window).
    mem.writeME1(0xB00C, 0x3C);
    CHECK(mem.readME1(0xB00C) == 0x3C);

    // The whole 0xB000-0xB00F ME1 block is the CE-150's LH5810 chip-select:
    // the non-register part (0xB000-0xB007) must read back as open bus, not
    // fall through to readME0() and get mis-served as CE-150 *ROM* bytes,
    // which would desync LPRINT's inter-step LH5810 poll after one glyph.
    mem.updatePUPV(false, /*pv=*/false); // ROM window would be live at PV=0
    for (uint16_t a = 0xB000; a <= 0xB007; ++a) CHECK(mem.readME1(a) == 0xFF);
    // ...while the ROM itself is still readable in the *ME0* window.
    CHECK(mem.readME0(0xB005) == rom[0xB005 - 0xA000]);

    // OPC writes (0xB008) step the carriage motor -- walk the ring forward 4.
    mem.writeME1(0xB008, 0x99); // anchor both nibbles at ring pos 0
    int pos = 0;
    for (int i = 0; i < 4; ++i) {
        pos = (pos + 1) % 8;
        mem.writeME1(0xB008, uint8_t(0x90 | kRing[pos]));
    }
    CHECK(m.ce150Card()->mechanism().penX() == 4);
}

void test_ce150_and_ce1600p_are_mutually_exclusive() {
    PC1600Machine m;
    auto ce150 = fakeRom(Ce150Card::kRomSize, 3);
    auto half1 = fakeRom(CE1600PCard::kRomHalfSize, 4);
    auto half2 = fakeRom(CE1600PCard::kRomHalfSize, 5);

    CHECK(m.attachCE150(ce150.data(), ce150.size()));
    CHECK(m.ce150Attached());
    CHECK(m.attachCE1600P(half1.data(), half1.size(), half2.data(), half2.size()));
    CHECK(m.ce1600pAttached());
    CHECK(!m.ce150Attached());               // attaching the CE-1600P dropped the CE-150

    CHECK(m.attachCE150(ce150.data(), ce150.size()));
    CHECK(m.ce150Attached());
    CHECK(!m.ce1600pAttached());             // ...and vice versa
}

void test_reset_reanchors_but_keeps_the_card() {
    PC1600Machine m;
    auto rom = fakeRom(Ce150Card::kRomSize, 6);
    m.attachCE150(rom.data(), rom.size());
    auto& mem = m.lh5803Memory();
    mem.writeME1(0xB008, 0x99);
    int pos = 0;
    for (int i = 0; i < 6; ++i) { pos = (pos + 1) % 8; mem.writeME1(0xB008, uint8_t(0x90 | kRing[pos])); }
    CHECK(m.ce150Card()->mechanism().penX() == 6);

    m.reset();
    CHECK(m.ce150Attached());                          // still plugged in
    CHECK(m.ce150Card()->mechanism().penX() == 0);     // re-anchored
    mem.updatePUPV(false, false);
    CHECK(mem.readME0(0xA000) == rom[0]);              // ROM still served
}

void test_lh5803_internal_pio_f00x_is_a_register_not_rom() {
    // The LH5803's on-chip LH5811-compat PIO at ME1 0xF000-0xF00F must be a
    // real register file, not a fall-through to readME0() (where 0xF00x >=
    // kRomBase serves PC1600-LH5803-C000-FFFF-new.bin / open-bus bytes). The CE-150 cartridge's
    // per-plot-point pacing poll is `BII #(0xF00B),0x02` via the system-ROM
    // helper at E451; if bit 1 reads set the draw loop unwinds after one
    // glyph.
    PC1600Machine m;
    auto rom = fakeRom(Ce150Card::kRomSize, 7);
    m.attachCE150(rom.data(), rom.size());
    auto& mem = m.lh5803Memory();

    // IF (0xF00B): starts zeroed; the pacing poll now sees bit 1 clear.
    CHECK(mem.readME1(0xF00B) == 0x00);
    CHECK((mem.readME1(0xF00B) & 0x02) == 0);

    // Every offset latches and reads straight back -- IF included, and with
    // no clear-on-read (the firmware clears bit 1 itself).
    mem.writeME1(0xF00C, 0xAB);
    CHECK(mem.readME1(0xF00C) == 0xAB);
    mem.writeME1(0xF00B, 0x02);
    CHECK(mem.readME1(0xF00B) == 0x02);
    CHECK(mem.readME1(0xF00B) == 0x02); // still set after a read

    // Independent of the CE-150's own LH5810 block at 0xB00x.
    mem.writeME1(0xB00C, 0x14);
    CHECK(mem.readME1(0xB00C) == 0x14);
    CHECK(mem.readME1(0xF00C) == 0xAB);

    // reset() clears the internal-PIO file (card stays attached).
    m.reset();
    CHECK(m.ce150Attached());
    CHECK(mem.readME1(0xF00C) == 0x00);
    CHECK(mem.readME1(0xF00B) == 0x00);
}

} // namespace

int run_pc1600_ce150_tests() {
    test_lh5803_rom_window_ignores_cpu_pv();
    test_lh5803_me1_block_and_motor_writes();
    test_ce150_and_ce1600p_are_mutually_exclusive();
    test_reset_reanchors_but_keeps_the_card();
    test_lh5803_internal_pio_f00x_is_a_register_not_rom();

    std::printf("pc1600_ce150_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
