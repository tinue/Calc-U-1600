// Headless C++ tests for PC1600Memory's Slot 1/2 RAM attachment
// (memory chips loaded into a slot). Same
// no-framework, assert-and-tally style as lh5801_tests.cpp -- see that
// file's header comment.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>

#include "../PC1600/PC1600Bank.hpp"
#include "../PC1600/PC1600Memory.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

void test_unattached_slots_are_open_bus() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    CHECK(!mem.slot1Attached());
    CHECK(!mem.slot2Attached());
    bank.writePort31(0x00); // pageCBank() == 0 (Slot 1)
    CHECK(mem.read(0x8000) == 0xFF);
    mem.write(0x8000, 0x42);
    CHECK(mem.read(0x8000) == 0xFF); // still open bus, write ignored
}

void test_attach_rejects_invalid_sizes() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    CHECK(!mem.attachSlot1(0));
    CHECK(!mem.attachSlot1(100)); // not a multiple of kBankSize
    CHECK(!mem.attachSlot1(3 * PC1600Memory::kBankSize)); // too large (> 2 banks)
    CHECK(!mem.slot1Attached());
}

void test_slot1_16k_readwrite_and_powerup_fill() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    CHECK(mem.attachSlot1(PC1600Memory::kBankSize));
    CHECK(mem.slot1Attached());
    bank.writePort31(0x00); // pageCBank() == 0
    CHECK(mem.read(0x8000) == 0xFF); // powers up 0xFF
    mem.write(0x8000, 0x11);
    CHECK(mem.read(0x8000) == 0x11);
    mem.write(0xBFFF, 0x22);
    CHECK(mem.read(0xBFFF) == 0x22);

    // Only 16KB attached -- bank 1 (the second half) must stay open bus.
    bank.writePort31(0x10); // pageCBank() == 1 (bits 4-6)
    CHECK(mem.read(0x8000) == 0xFF);
}

void test_slot1_32k_two_banks() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    CHECK(mem.attachSlot1(2 * PC1600Memory::kBankSize));
    bank.writePort31(0x00); // bank 0
    mem.write(0x8000, 0xAA);
    bank.writePort31(0x10); // bank 1 (bits 4-6)
    mem.write(0x8000, 0xBB);
    CHECK(mem.read(0x8000) == 0xBB);
    bank.writePort31(0x00);
    CHECK(mem.read(0x8000) == 0xAA); // independent from bank 1's byte
}

void test_slot2_plain_card_aliases_across_vertical_banks() {
    // The connector does not gate Slot 2 on Port 28H vertical bank -- that
    // is the attached card's concern (a CE-1601M-class card tracks it via
    // its own latch). A plain RAM card that doesn't decode Port 28H simply
    // presents the same bytes for every vertical bank, which is what such a
    // module really does on hardware.
    PC1600Bank bank;
    PC1600Memory mem(bank);
    CHECK(mem.attachSlot2(PC1600Memory::kBankSize));
    bank.writePort31(static_cast<uint8_t>(2 << 4)); // pageCBank() == 2 (Slot 2)
    bank.writePort28(0);
    mem.write(0x8000, 0x55);
    CHECK(mem.read(0x8000) == 0x55);

    bank.writePort28(1); // a plain card ignores Port 28H -> same byte
    CHECK(mem.read(0x8000) == 0x55);
    mem.write(0x8000, 0x99);
    bank.writePort28(0);
    CHECK(mem.read(0x8000) == 0x99); // the write at vb1 hit the same cell
}

void test_detach_reverts_to_open_bus() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    mem.attachSlot1(PC1600Memory::kBankSize);
    bank.writePort31(0x00);
    mem.write(0x8000, 0x11);
    mem.detachSlot1();
    CHECK(!mem.slot1Attached());
    CHECK(mem.read(0x8000) == 0xFF);
}

} // namespace

int run_pc1600_slot_ram_tests() {
    test_unattached_slots_are_open_bus();
    test_attach_rejects_invalid_sizes();
    test_slot1_16k_readwrite_and_powerup_fill();
    test_slot1_32k_two_banks();
    test_slot2_plain_card_aliases_across_vertical_banks();
    test_detach_reverts_to_open_bus();

    std::printf("pc1600_slot_ram_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
