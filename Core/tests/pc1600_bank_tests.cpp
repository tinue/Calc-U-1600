// Headless C++ tests for the PC-1600 bank-switching layer
// (Core/PC1600/PC1600Bank.hpp, PC1600Memory.hpp). Pure truth-table tests
// against synthetic port writes and byte patterns -- no CPU involved.
// Same no-framework, assert-and-tally style as lh5801_tests.cpp -- see
// that file's header comment.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <vector>

#include "../PC1600/PC1600Bank.hpp"
#include "../PC1600/PC1600Memory.hpp"

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

// ── Port 31H: four independent bit fields ────────────────────────────────

void test_port31_page_a_bank_is_bit0_only() {
    PC1600Bank bank;
    bank.writePort31(0x00);
    CHECK(bank.pageABank() == 0);
    bank.writePort31(0x01);
    CHECK(bank.pageABank() == 1);
    // Every other bit set, bit 0 clear -- must not leak into page A.
    bank.writePort31(0xFE);
    CHECK(bank.pageABank() == 0);
}

void test_port31_page_b_bank_is_bits_1_to_3() {
    PC1600Bank bank;
    for (uint8_t v = 0; v <= 7; v++) {
        bank.writePort31(static_cast<uint8_t>(v << 1));
        CHECK(bank.pageBBank() == v);
    }
    // Bits outside 1-3 must not leak in.
    bank.writePort31(0xF1); // b0=1, b4-7=1, b1-3=0
    CHECK(bank.pageBBank() == 0);
}

void test_port31_page_c_bank_is_bits_4_to_6() {
    PC1600Bank bank;
    for (uint8_t v = 0; v <= 7; v++) {
        bank.writePort31(static_cast<uint8_t>(v << 4));
        CHECK(bank.pageCBank() == v);
    }
    bank.writePort31(0x8F); // b7=1, b0-3=1, b4-6=0
    CHECK(bank.pageCBank() == 0);
}

void test_port31_page_d_bank_is_bit7_only() {
    PC1600Bank bank;
    bank.writePort31(0x00);
    CHECK(bank.pageDBank() == 0);
    bank.writePort31(0x80);
    CHECK(bank.pageDBank() == 1);
    bank.writePort31(0x7F); // every other bit set, bit 7 clear
    CHECK(bank.pageDBank() == 0);
}

void test_port31_fields_are_fully_independent() {
    // Set every field to a distinct, recognizable value in one write and
    // confirm none of the four fields bleeds into another.
    PC1600Bank bank;
    // pageA=1(b0), pageB=5(b1-3=101), pageC=3(b4-6=011), pageD=1(b7)
    // = 1 + (5<<1) + (3<<4) + (1<<7) = 1 + 10 + 48 + 128 = 187 = 0xBB
    bank.writePort31(0xBB);
    CHECK(bank.pageABank() == 1);
    CHECK(bank.pageBBank() == 5);
    CHECK(bank.pageCBank() == 3);
    CHECK(bank.pageDBank() == 1);
    CHECK(bank.readPort31() == 0xBB);
}

// ── Port 28H: Slot 2 vertical bank ────────────────────────────────────────

void test_port28_write_readback_round_trip() {
    PC1600Bank bank;
    CHECK(bank.slot2VerticalBank() == 0);
    bank.writePort28(5);
    CHECK(bank.slot2VerticalBank() == 5);
    bank.writePort28(255); // superRAM-range value; no range check at this layer
    CHECK(bank.slot2VerticalBank() == 255);
}

// ── Port 3CH: SLOT1MAP / SLOT2MAP mode ────────────────────────────────────

void test_port3c_slot2map_mode_decode() {
    PC1600Bank bank;
    CHECK(bank.slot2MapMode() == 0);   // power-up: default page-C banks 2/3
    bank.writePort3C(0x20);            // b5 -> SLOT2MAP A=1
    CHECK(bank.slot2MapMode() == 1);
    bank.writePort3C(0x10);            // b4 -> SLOT2MAP A=2
    CHECK(bank.slot2MapMode() == 2);
    bank.writePort3C(0x30);            // SLOT2MAP never writes both; b5 wins
    CHECK(bank.slot2MapMode() == 1);
    bank.writePort3C(0x1B);            // the boot value -> A=2 (b4 set, b5 clear)
    CHECK(bank.slot2MapMode() == 2);
    bank.writePort3C(0x04);            // SLOT1MAP bit only -- no SLOT2MAP effect
    CHECK(bank.slot2MapMode() == 0);
    CHECK(bank.readPort3C() == 0x04);
    CHECK(bank.slot1MapActive());
}

void test_port3c_slot1map_active() {
    PC1600Bank bank;
    CHECK(!bank.slot1MapActive());     // power-up: default, no mirror
    bank.writePort3C(0x04);
    CHECK(bank.slot1MapActive());
    bank.writePort3C(0x00);
    CHECK(!bank.slot1MapActive());
    bank.writePort3C(0xFB);            // every bit set except b2
    CHECK(!bank.slot1MapActive());
}

// "Last call wins" arbitration for the SLOT1MAP=1 / SLOT2MAP-mode-2
// collision at page-B bank 1: whichever field's *value* most recently
// changed wins; an exact tie (both change in the same write) favors
// SLOT2MAP. See PC1600Bank::slot1MapWinsTie() / resolveSlotCollision().
void test_port3c_slot1map_wins_tie() {
    PC1600Bank bank;
    CHECK(!bank.slot1MapWinsTie());    // neither ever changed -> SLOT2MAP default

    bank.writePort3C(0x14);            // both bits set in the same write: exact tie
    CHECK(!bank.slot1MapWinsTie());    // SLOT2MAP wins the tie

    bank.writePort3C(0x00);
    bank.writePort3C(0x10);            // SLOT2MAP field set first
    bank.writePort3C(0x14);            // SLOT1MAP bit added, SLOT2MAP field unchanged
    CHECK(bank.slot1MapWinsTie());     // SLOT1MAP is the more recent change

    bank.writePort3C(0x00);
    bank.writePort3C(0x04);            // SLOT1MAP bit set first
    bank.writePort3C(0x14);            // SLOT2MAP field added, SLOT1MAP bit unchanged
    CHECK(!bank.slot1MapWinsTie());    // SLOT2MAP is now the more recent change

    // A write that doesn't touch either field at all leaves the
    // last-changed bookkeeping, and so the tie-break outcome, unaffected.
    bank.writePort3C(0x14);
    CHECK(bank.readPort3C() == 0x14);
    bank.writePort3D(0xFF);            // an unrelated port
    CHECK(!bank.slot1MapWinsTie());    // still SLOT2MAP's turn
}

// ── Port 3DH: hidden-ROM latch ────────────────────────────────────────────

void test_port3d_hidden_rom_latch() {
    PC1600Bank bank;
    bank.writePort3D(0x04); // bit 2 set -> normal ROM
    CHECK(!bank.hiddenBasicRomSelected());
    bank.writePort3D(0x00); // bit 2 clear -> hidden ROM
    CHECK(bank.hiddenBasicRomSelected());
    bank.writePort3D(0xFB); // every bit set except bit 2
    CHECK(bank.hiddenBasicRomSelected());
    bank.writePort3D(0xFF); // every bit set, including bit 2
    CHECK(!bank.hiddenBasicRomSelected());
}

void test_reset_zeroes_all_bank_registers() {
    PC1600Bank bank;
    bank.writePort31(0xBB);
    bank.writePort28(7);
    bank.writePort3C(0x30);
    bank.writePort3D(0x00);
    bank.reset();
    CHECK(bank.readPort31() == 0);
    CHECK(bank.slot2VerticalBank() == 0);
    CHECK(bank.readPort3C() == 0);
    CHECK(bank.slot2MapMode() == 0);
    CHECK(bank.hiddenBasicRomSelected()); // port3D==0 -> bit2 clear -> hidden selected
}

// ── PC1600Memory: page A always resident regardless of pageABank() ──────

void test_page_a_always_resident_regardless_of_port31_bit0() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    std::vector<uint8_t> lower = makeBank(0x11);
    std::vector<uint8_t> upper = makeBank(0x22);
    CHECK(mem.loadBank0(lower.data(), lower.size(), upper.data(), upper.size()));

    bank.writePort31(0x00); // page A bank 0
    CHECK(mem.read(0x0000) == 0x11);
    bank.writePort31(0x01); // page A bank 1 (undocumented content) -- still
                             // resolves to the same, only-ever-loaded ROM
    CHECK(mem.read(0x0000) == 0x11);
}

void test_page_b_bank0_is_system_rom_upper_half() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    std::vector<uint8_t> lower = makeBank(0x11);
    std::vector<uint8_t> upper = makeBank(0x22);
    mem.loadBank0(lower.data(), lower.size(), upper.data(), upper.size());
    bank.writePort31(0x00); // pageBBank() == 0
    CHECK(mem.read(0x4000) == 0x22);
    CHECK(mem.read(0x7FFF) == 0x22);
}

void test_page_b_unbacked_banks_are_open_bus() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    bank.writePort31(static_cast<uint8_t>(1 << 1)); // pageBBank() == 1, never loaded
    CHECK(mem.read(0x4000) == 0xFF);
}

void test_hidden_rom_latch_selects_bank3_vs_bank3b() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    std::vector<uint8_t> bank3 = makeBank(0x33);
    std::vector<uint8_t> bank3b = makeBank(0x3B);
    CHECK(mem.loadBank3Rom(bank3.data(), bank3.size()));
    CHECK(mem.loadBank3bRom(bank3b.data(), bank3b.size()));

    bank.writePort31(static_cast<uint8_t>(3 << 1)); // pageBBank() == 3
    bank.writePort3D(0x04); // bit2 set -> normal Bank 3
    CHECK(mem.read(0x4000) == 0x33);
    bank.writePort3D(0x00); // bit2 clear -> hidden Bank 3b
    CHECK(mem.read(0x4000) == 0x3B);
}

void test_page_c_bank6_display_timer_serial_char_rom() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    std::vector<uint8_t> bank6 = makeBank(0x66);
    CHECK(mem.loadBank6Rom(bank6.data(), bank6.size()));
    bank.writePort31(static_cast<uint8_t>(6 << 4)); // pageCBank() == 6
    CHECK(mem.read(0x8000) == 0x66);
    CHECK(mem.read(0xBFFF) == 0x66);
}

void test_page_c_other_banks_are_open_bus_in_v1() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    for (uint8_t b = 0; b <= 7; b++) {
        if (b == 6) continue;
        bank.writePort31(static_cast<uint8_t>(b << 4));
        CHECK(mem.read(0x8000) == 0xFF);
    }
}

void test_page_d_bank0_is_internal_ram_readwrite() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    bank.writePort31(0x00); // pageDBank() == 0
    mem.write(0xC000, 0xAB);
    CHECK(mem.read(0xC000) == 0xAB);
    mem.write(0xFFFF, 0xCD);
    CHECK(mem.read(0xFFFF) == 0xCD);
}

void test_page_d_bank1_is_open_bus_and_ignores_writes() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    bank.writePort31(0x80); // pageDBank() == 1
    mem.write(0xC000, 0xAB); // must be silently ignored (open bus)
    CHECK(mem.read(0xC000) == 0xFF);
}

void test_rom_ignores_writes() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    std::vector<uint8_t> lower = makeBank(0x11);
    std::vector<uint8_t> upper = makeBank(0x22);
    mem.loadBank0(lower.data(), lower.size(), upper.data(), upper.size());
    mem.write(0x0000, 0x99);
    CHECK(mem.read(0x0000) == 0x11); // unchanged
}

void test_port3d_f07dh_mirror() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    bank.writePort31(0x00); // pageDBank() == 0, so 0xF07D is in RAM range
    bank.writePort3D(0x05);
    CHECK(mem.read(PC1600Memory::kPort3DMirrorAddr) == 0x05);
    bank.writePort3D(0xFA);
    CHECK(mem.read(PC1600Memory::kPort3DMirrorAddr) == 0xFA);
}

void test_reset_keeps_ram_all_reset_clears_it_never_touches_rom() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    std::vector<uint8_t> lower = makeBank(0x11);
    std::vector<uint8_t> upper = makeBank(0x22);
    mem.loadBank0(lower.data(), lower.size(), upper.data(), upper.size());
    bank.writePort31(0x00);
    mem.write(0xC000, 0xAB);

    // Simple reset keeps internal RAM (the ROM's warm-start path needs it).
    mem.reset();
    CHECK(mem.read(0xC000) == 0xAB);
    CHECK(mem.read(0x0000) == 0x11); // ROM untouched

    // ALL RESET wipes internal RAM; ROM still untouched.
    mem.clearInternalRam();
    mem.reset();
    CHECK(mem.read(0xC000) == 0x00);
    CHECK(mem.read(0x0000) == 0x11);
}

void test_load_rejects_wrong_size() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    std::vector<uint8_t> tooShort(100, 0);
    CHECK(!mem.loadBank3Rom(tooShort.data(), tooShort.size()));
    CHECK(!mem.loadBank3bRom(tooShort.data(), tooShort.size()));
    CHECK(!mem.loadBank6Rom(tooShort.data(), tooShort.size()));
    CHECK(!mem.loadBank0(tooShort.data(), tooShort.size(), tooShort.data(), tooShort.size()));
}

} // namespace

// Defined here, called from lh5801_tests.cpp's main() -- same
// single-binary convention as run_connector_tests() etc.
int run_pc1600_bank_tests() {
    test_port31_page_a_bank_is_bit0_only();
    test_port31_page_b_bank_is_bits_1_to_3();
    test_port31_page_c_bank_is_bits_4_to_6();
    test_port31_page_d_bank_is_bit7_only();
    test_port31_fields_are_fully_independent();
    test_port28_write_readback_round_trip();
    test_port3c_slot2map_mode_decode();
    test_port3c_slot1map_active();
    test_port3c_slot1map_wins_tie();
    test_port3d_hidden_rom_latch();
    test_reset_zeroes_all_bank_registers();
    test_page_a_always_resident_regardless_of_port31_bit0();
    test_page_b_bank0_is_system_rom_upper_half();
    test_page_b_unbacked_banks_are_open_bus();
    test_hidden_rom_latch_selects_bank3_vs_bank3b();
    test_page_c_bank6_display_timer_serial_char_rom();
    test_page_c_other_banks_are_open_bus_in_v1();
    test_page_d_bank0_is_internal_ram_readwrite();
    test_page_d_bank1_is_open_bus_and_ignores_writes();
    test_rom_ignores_writes();
    test_port3d_f07dh_mirror();
    test_reset_keeps_ram_all_reset_clears_it_never_touches_rom();
    test_load_rejects_wrong_size();

    std::printf("pc1600_bank_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
