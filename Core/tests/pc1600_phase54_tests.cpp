// Headless C++ tests for the remaining PC-1600 ROM images:
// PC1600-P1-B3-new.bin/PC1600-P1-B3B-new.bin/PC1600-P2-B6-new.bin wired into PC1600Machine at their
// documented bank addresses, and PC1600-P1-B4-CE1600P.bin/PC1600-P1-B5-CE1600P-OR-F.bin
// (confirmed CE-1600P ROM -- see roms/README.md) wired in via PC1600Machine::attachCE1600P().
// Same no-framework, assert-and-tally style as lh5801_tests.cpp -- see that
// file's header comment.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <vector>

#include "../PC1600/PC1600Machine.hpp"
#include "TestRoms.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// Real ROM files loaded via loadBank3Rom/loadBank3bRom, then exercised
// through actual SC7852 execution (LD A,(4000H) ; HALT) rather than just
// checking the raw bytes loaded correctly -- confirms the hidden-ROM latch
// (Port 3DH bit 2) actually switches which of the two loaded images page B
// resolves to.
void test_hidden_rom_latch_via_execution() {
    PC1600Machine m;
    std::vector<uint8_t> bank3, bank3b;
    if (!readRomImage("roms/PC1600-P1-B3-new.bin", &bank3) ||
        !readRomImage("roms/PC1600-P1-B3B-new.bin", &bank3b)) {
        std::fprintf(stderr, "SKIP test_hidden_rom_latch_via_execution: "
                              "roms/PC1600-P1-B3-new.bin or PC1600-P1-B3B-new.bin not found "
                              "relative to cwd (run tests from the repo root)\n");
        return;
    }
    CHECK(bank3[0] != bank3b[0]); // prerequisite for the latch to be observable at all

    std::vector<uint8_t> lower(16384, 0x00), upper(16384, 0x00);
    lower[0] = 0x3A; lower[1] = 0x00; lower[2] = 0x40; // LD A,(4000H)
    lower[3] = 0x76;                                    // HALT
    CHECK(m.loadBank0(lower.data(), lower.size(), upper.data(), upper.size()));
    CHECK(m.loadBank3Rom(bank3.data(), bank3.size()));
    CHECK(m.loadBank3bRom(bank3b.data(), bank3b.size()));

    m.reset();
    m.bank().writePort31(static_cast<uint8_t>(3 << 1)); // pageBBank() == 3
    m.bank().writePort3D(0x04);                           // bit2 set -> normal Bank 3
    m.step();                                             // LD A,(4000H)
    CHECK(m.sc7852().a() == bank3[0]);

    m.reset();
    m.bank().writePort31(static_cast<uint8_t>(3 << 1));
    m.bank().writePort3D(0x00); // bit2 clear -> hidden Bank 3b
    m.step();
    CHECK(m.sc7852().a() == bank3b[0]);
}

void test_bank6_display_timer_serial_char_rom_load() {
    PC1600Machine m;
    std::vector<uint8_t> bank6;
    if (!readRomImage("roms/PC1600-P2-B6-new.bin", &bank6)) {
        std::fprintf(stderr, "SKIP test_bank6_display_timer_serial_char_rom_load: "
                              "roms/PC1600-P2-B6-new.bin not found\n");
        return;
    }
    std::vector<uint8_t> lower(16384, 0x00), upper(16384, 0x00);
    lower[0] = 0x3A; lower[1] = 0x00; lower[2] = 0x80; // LD A,(8000H)
    lower[3] = 0x76;                                     // HALT
    CHECK(m.loadBank0(lower.data(), lower.size(), upper.data(), upper.size()));
    CHECK(m.loadBank6Rom(bank6.data(), bank6.size()));
    m.reset();
    m.bank().writePort31(static_cast<uint8_t>(6 << 4)); // pageCBank() == 6
    m.step();
    CHECK(m.sc7852().a() == bank6[0]);
}

// CE-1600P ROM (PC1600-P1-B4-CE1600P.bin/PC1600-P1-B5-CE1600P-OR-F.bin,
// confirmed -- see roms/README.md) exercised through real SC7852 execution reading Page B
// banks 4/5, exactly like test_hidden_rom_latch_via_execution() above does
// for bank 3/3b -- and confirming those banks are still open bus before
// attach / after detach, and that unrelated banks (1, 2, 7) stay open bus
// once a CE-1600P is attached (its ROM only answers on 4/5).
void test_ce1600p_rom_attach_and_open_bus() {
    PC1600Machine m;
    std::vector<uint8_t> ce1, ce2;
    if (!readRomImage("roms/PC1600-P1-B4-CE1600P.bin", &ce1) ||
        !readRomImage("roms/PC1600-P1-B5-CE1600P-OR-F.bin", &ce2)) {
        std::fprintf(stderr, "SKIP test_ce1600p_rom_attach_and_open_bus: "
                              "roms/PC1600-P1-B4-CE1600P.bin or PC1600-P1-B5-CE1600P-OR-F.bin not found\n");
        return;
    }

    std::vector<uint8_t> lower(16384, 0x00), upper(16384, 0x00);
    lower[0] = 0x3A; lower[1] = 0x00; lower[2] = 0x40; // LD A,(4000H)
    lower[3] = 0x76;                                     // HALT
    CHECK(m.loadBank0(lower.data(), lower.size(), upper.data(), upper.size()));

    // Before attach: banks 4/5 are open bus, same as every other
    // undocumented Page B bank.
    m.reset();
    m.bank().writePort31(static_cast<uint8_t>(4 << 1));
    m.step(); m.step();
    CHECK(m.sc7852().a() == 0xFF);

    CHECK(m.attachCE1600P(ce1.data(), ce1.size(), ce2.data(), ce2.size()));
    CHECK(m.ce1600pAttached());

    m.reset();
    m.bank().writePort31(static_cast<uint8_t>(4 << 1)); // pageBBank() == 4
    m.step(); m.step();
    CHECK(m.sc7852().a() == ce1[0]);

    m.reset();
    m.bank().writePort31(static_cast<uint8_t>(5 << 1)); // pageBBank() == 5
    m.step(); m.step();
    CHECK(m.sc7852().a() == ce2[0]);

    // Banks 1/2/7 remain open bus with CE-1600P attached -- it only claims
    // 4/5.
    for (uint8_t pageBBank : {1, 2, 7}) {
        m.reset();
        m.bank().writePort31(static_cast<uint8_t>(pageBBank << 1));
        m.step(); m.step();
        CHECK(m.sc7852().a() == 0xFF);
    }

    m.detachCE1600P();
    CHECK(!m.ce1600pAttached());
    m.reset();
    m.bank().writePort31(static_cast<uint8_t>(4 << 1));
    m.step(); m.step();
    CHECK(m.sc7852().a() == 0xFF); // detached: open bus again
}

} // namespace

int run_pc1600_phase54_tests() {
    test_hidden_rom_latch_via_execution();
    test_bank6_display_timer_serial_char_rom_load();
    test_ce1600p_rom_attach_and_open_bus();

    std::printf("pc1600_phase54_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
