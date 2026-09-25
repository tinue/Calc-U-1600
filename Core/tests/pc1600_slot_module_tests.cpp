// Headless C++ tests for real module definitions (the bundled
// Qt6/resources/cards/*.card.yaml, as SoftwareDefinedCard) plugged into a
// PC-1600 40-pin memory-slot connector (Core/Connector/MemorySlotConnector.hpp),
// end to end through PC1600Machine. The card is the *same* definition the
// PC-1500 side uses -- it wires in pin-for-pin and the connector drives the
// PC-1600 bay's pins. Same no-framework,
// assert-and-tally style as lh5801_tests.cpp.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
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

// A bundled module definition. They ship in the repo, so a missing or
// unloadable one is a test failure, not a SKIP.
std::unique_ptr<SoftwareDefinedCard> card(const char* file, CardHost host) {
    auto c = bundledCard(file, host);
    CHECK(c != nullptr);
    return c;
}

// Select which page-C (8000-BFFF) bank the SC7852 sees: value 0/1 -> Slot 1,
// 2/3 -> Slot 2 (Port 31H bits 4-6).
void selectPageCBank(PC1600Machine& m, uint8_t bank) {
    m.memory().writeIO(0x31, uint8_t(bank << 4));
}

void test_ce155_in_slot1_both_cpu_views() {
    auto ce155 = card("ce155.card.yaml", CardHost::PC1600Slot1);
    if (!ce155) return;
    PC1600Machine m;
    m.attachSlot1Card(std::move(ce155));
    CHECK(m.slot1Attached());

    selectPageCBank(m, 0); // Slot 1, bank 0 -> RAM2# asserted at 8000-BFFF

    // A bare CE-155 in a PC-1600 Slot 1 tiles a contiguous 8KB at A000-BFFF:
    // its chips 1/2/3 wire to pins 16/17/18 (the mainboard's S1/S2/S3
    // sub-selects for A000-A7FF / A800-AFFF / B000-B7FF), and its onboard-
    // decoded chip 0 (pin 4 + AD11-AD13 = 111) covers the top block
    // B800-BFFF. This matches stock real hardware (MEM +8192, RAM base
    // A0C5H -- the documented CE-159 figure).
    m.memory().write(0xA000, 0x10); // chip 1, low edge
    m.memory().write(0xA800, 0x11); // chip 2
    m.memory().write(0xB000, 0x22); // chip 3
    m.memory().write(0xB800, 0x5A); // chip 0
    m.memory().write(0xBFFF, 0xA5);
    CHECK(m.memory().read(0xA000) == 0x10);
    CHECK(m.memory().read(0xA800) == 0x11);
    CHECK(m.memory().read(0xB000) == 0x22);
    CHECK(m.memory().read(0xB800) == 0x5A);
    CHECK(m.memory().read(0xBFFF) == 0xA5);

    // The LH5803's 0000-3FFF window aliases the Z-80's 8000-BFFF (+0x8000),
    // backed by the same card -- so &2000 on the LH5803 side is &A000 here.
    CHECK(m.lh5803Memory().readME0(0x2000) == 0x10);
    m.lh5803Memory().writeME0(0x2001, 0x33);
    CHECK(m.memory().read(0xA001) == 0x33);

    // Below the module (8000-9FFF): open bus.
    CHECK(m.memory().read(0x8000) == 0xFF);
    m.memory().write(0x8000, 0x99);
    CHECK(m.memory().read(0x8000) == 0xFF);

    // Bank switched away from Slot 1 (page-C bank 4 = open bus in v1):
    // RAM2# deasserts, every select line goes inactive, nothing responds.
    selectPageCBank(m, 4);
    CHECK(m.memory().read(0xB800) == 0xFF);
    CHECK(m.memory().read(0xA000) == 0xFF);
    selectPageCBank(m, 0);
    CHECK(m.memory().read(0xB800) == 0x5A); // content survived the excursion
    CHECK(m.memory().read(0xA000) == 0x10);

    // Detach: slot reverts to open bus.
    m.detachSlot1();
    CHECK(!m.slot1Attached());
    CHECK(m.memory().read(0xB800) == 0xFF);
}

void test_plain_ram_card_via_attach_slot_card() {
    PC1600Machine m;
    m.attachSlot1Card(plainRamCard(0x8000)); // 32KB, two banks
    CHECK(m.slot1Attached());

    selectPageCBank(m, 0); // Slot 1 bank 0 -> low 16KB half (PVOUT = 0)
    m.memory().write(0x8000, 0x11);
    m.memory().write(0xBFFF, 0x22);
    CHECK(m.memory().read(0x8000) == 0x11);
    CHECK(m.memory().read(0xBFFF) == 0x22);

    selectPageCBank(m, 1); // Slot 1 bank 1 -> high 16KB half (PVOUT = 1)
    CHECK(m.memory().read(0x8000) == 0x00); // distinct region, still powered-up
    m.memory().write(0x8000, 0xEE);
    CHECK(m.memory().read(0x8000) == 0xEE);

    selectPageCBank(m, 0);
    CHECK(m.memory().read(0x8000) == 0x11); // bank 0's byte intact

    // poke() reaches the card too (host/debug path).
    m.memory().poke(0x9000, 0x7F);
    CHECK(m.memory().read(0x9000) == 0x7F);
    CHECK(m.memory().isWritable(0x9000));
}

// pokeMemory() is a host poke: it lands in flash unconditionally (the
// direct path, not the flash command decoder), and a region that claims
// the write but keeps nothing makes it fail without changing anything.
void test_poke_memory_uses_the_host_path_and_verifies() {
    {
        PC1600Machine m;
        auto card = slotCardWithContent(
            "{ kind: flash, protocol: { unlock-sequence: [ { address: 0x555, data: 0xAA }, "
            "{ address: 0x2AA, data: 0x55 } ], byte-program-command: 0xA0, "
            "erase-setup-command: 0x80, sector-erase-command: 0x30, chip-erase-command: 0x10, "
            "reset-command: 0xF0, sector-size: 0x1000 } }");
        CHECK(card != nullptr);
        if (!card) return;
        m.attachSlot1Card(std::move(card));
        selectPageCBank(m, 0);
        const uint8_t bytes[] = {0x12, 0x34};
        CHECK(m.pokeMemory(0x8100, bytes, 2));
        CHECK(m.memory().read(0x8100) == 0x12);
        CHECK(m.memory().read(0x8101) == 0x34);
    }
    {
        PC1600Machine m;
        auto card = slotCardWithContent("{ kind: regular, writable: false, power-up-fill: 0x5A }");
        CHECK(card != nullptr);
        if (!card) return;
        m.attachSlot1Card(std::move(card));
        selectPageCBank(m, 0);
        const uint8_t bytes[] = {0x12, 0x34};
        CHECK(!m.pokeMemory(0x8100, bytes, 2));
        CHECK(m.memory().read(0x8100) == 0x5A);
        CHECK(m.memory().read(0x8101) == 0x5A);
    }
    {
        // Partly writable: the range crosses from internal RAM (FFFF) into
        // ROM (0000) -- rejected up front, the RAM byte left alone.
        PC1600Machine m;
        m.memory().poke(0xFFFF, 0x77);
        const uint8_t bytes[] = {0x12, 0x34};
        CHECK(!m.pokeMemory(0xFFFF, bytes, 2));
        CHECK(m.memory().read(0xFFFF) == 0x77);
    }
}

void test_slot2_plain_card_ignores_vertical_bank_through_connector() {
    // The connector selects Slot 2 regardless of the Port 28H vertical bank
    // (the vertical bank is the card's own business -- a CE-1601M-class
    // card latches it from OUT (28H)). A plain RAM card does not decode
    // Port 28H, so it aliases across vertical banks.
    PC1600Machine m;
    m.attachSlot2Card(plainRamCard(0x4000)); // 16KB in Slot 2
    CHECK(m.slot2Attached());

    m.memory().writeIO(0x31, uint8_t(2 << 4)); // page-C bank 2 -> Slot 2
    m.memory().writeIO(0x28, 0);
    m.memory().write(0x8000, 0x44);
    CHECK(m.memory().read(0x8000) == 0x44);

    m.memory().writeIO(0x28, 1); // plain card ignores Port 28H
    CHECK(m.memory().read(0x8000) == 0x44);
    m.memory().write(0x8000, 0x55);
    m.memory().writeIO(0x28, 0);
    CHECK(m.memory().read(0x8000) == 0x55); // the vb1 write hit the same cell
}

// With the real ROM set booting, a Slot 1 module driving the LHS1-3
// sub-select lines must not wedge or corrupt the boot -- and its RAM stays
// reachable through the running machine. (Not an assertion about `MEM`
// growth: a bare CE-155 tiles A000-BFFF and the ROM credits its full 8KB
// -- verified separately against the documented real-hardware figure.)
void test_boot_with_ce155_in_slot1_is_stable() {
    PC1600Machine m;
    if (!loadPC1600Roms(m)) {
        std::fprintf(stderr, "SKIP test_boot_with_ce155_in_slot1_is_stable: PC-1600 ROM images not found\n");
        return;
    }
    auto ce155 = card("ce155.card.yaml", CardHost::PC1600Slot1);
    if (!ce155) return;
    m.attachSlot1Card(std::move(ce155));
    m.allReset();
    // Past the boot sequence -- same bar as tools/pc1600_cli.cpp's own
    // "converges to a stable PC set" (~2M T-states).
    m.runCycles(PC1600Machine::kTStateHz * 2);

    // The machine is alive: stepping a bit more doesn't fault.
    uint64_t ran = m.runCycles(PC1600Machine::kTStateHz / 4);
    CHECK(ran > 0);

    // The CE-155's RAM is reachable post-boot across A000-BFFF, both CPU views.
    m.memory().writeIO(0x31, m.memory().readIO(0x31) & 0x8F); // page-C bank -> 0 (Slot 1), keep other fields
    m.memory().write(0xA000, 0x77);
    CHECK(m.memory().read(0xA000) == 0x77);
    CHECK(m.lh5803Memory().readME0(0x2000) == 0x77); // LH5803 &2000 == Z-80 &A000
}

// The boot ROM's own memory sizing must credit the CE-155 its full 8KB:
// with the module the BASIC RAM base drops from C0C5H to A0C5H (the
// documented CE-159 figure -- PC-1600-Memory-Architecture.md), i.e. every
// RAM-base/size work-area pointer moves by exactly 8192.
void test_ce155_contributes_full_8k_to_mem() {
    auto boot = [](bool withCard) -> std::unique_ptr<PC1600Machine> {
        auto m = std::make_unique<PC1600Machine>();
        if (!loadPC1600Roms(*m)) return nullptr;
        if (withCard) m->attachSlot1Card(card("ce155.card.yaml", CardHost::PC1600Slot1));
        m->allReset();
        m->runCycles(PC1600Machine::kTStateHz * 4);
        return m;
    };
    auto m0 = boot(false), m1 = boot(true);
    if (!m0 || !m1) {
        std::fprintf(stderr, "SKIP test_ce155_contributes_full_8k_to_mem: PC-1600 ROM images not found\n");
        return;
    }
    auto rd16 = [](PC1600Machine& m, uint16_t a) {
        return uint16_t(m.memory().read(a) | (m.memory().read(uint16_t(a + 1)) << 8));
    };
    // F5CFH holds the BASIC RAM base pointer (C0C5H stock).
    CHECK(rd16(*m0, 0xF5CF) == 0xC0C5);
    CHECK(rd16(*m1, 0xF5CF) == 0xA0C5); // stock - 8192, program area start A0C5H
    CHECK(int(rd16(*m0, 0xF89D)) - int(rd16(*m1, 0xF89D)) == 8192);
}

// The CE-1638 plugged into a PC-1600 Slot 1: the same definition the
// PC-1500 `memory-expansion:` path builds, wired in pin-for-pin. Its pin-4 chip select covers &8000-&BFFF here (not a
// PC-1500's &0000-&3FFF Y0), so the card's banked-window index is masked to
// the 16KB bank size -- &8000 and &BFFF must land at opposite ends of one
// bank, not alias. Bank switching is the pin-18 write strobe (PC-1600 Slot
// 1 S3), bank number from address bits A0-A2 of the strobing write.
void test_ce1638_in_slot1_banked_window() {
    auto ce1638 = card("ce1638.card.yaml", CardHost::PC1600Slot1);
    if (!ce1638) return;
    PC1600Machine m;
    m.attachSlot1Card(std::move(ce1638));
    CHECK(m.slot1Attached());

    selectPageCBank(m, 0); // Slot 1, bank 0 -> RAM2# asserted at 8000-BFFF

    // Bank 0: the window's two ends are distinct 16KB offsets, not aliases
    // -- this is what the address mask buys on a PC-1600 slot.
    m.memory().write(0x8000, 0xAA);
    m.memory().write(0xBFFF, 0x77);
    CHECK(m.memory().read(0x8000) == 0xAA);
    CHECK(m.memory().read(0xBFFF) == 0x77);

    // Strobe pin 18 (any write into &B000-&B7FF): A0-A2 = 5 -> bank 5.
    m.memory().write(0xB005, 0x00);
    CHECK(m.memory().read(0x8000) == 0x00); // bank 5, powered-up, own contents
    m.memory().write(0x8000, 0xBB);
    CHECK(m.memory().read(0x8000) == 0xBB);

    // Back to bank 0 -- its bytes survived the excursion.
    m.memory().write(0xB000, 0x00);
    CHECK(m.memory().read(0x8000) == 0xAA);
    CHECK(m.memory().read(0xBFFF) == 0x77);

    m.detachSlot1();
    CHECK(m.memory().read(0x8000) == 0xFF); // slot reverts to open bus
}

// The CE-163F (8 RAM banks + 8 FLASH banks) in a PC-1600
// Slot 1: RAM banks are plain R/W, a FLASH bank ignores a bare CPU store
// but takes the JEDEC unlock + byte-program sequence, and a host poke()
// (direct write) bypasses the lock. The unlock command addresses match on
// the low 11 bits of the *masked* window offset, so &9555/&AAAA on the
// slot hit the same 0x555/0x2AA the firmware uses at &1555/&2AAA on a
// PC-1500.
void test_ce163f_in_slot1_ram_and_flash_protocol() {
    auto ce163f = card("ce163f.card.yaml", CardHost::PC1600Slot1);
    if (!ce163f) return;
    PC1600Machine m;
    m.attachSlot1Card(std::move(ce163f));
    CHECK(m.slot1Attached());
    selectPageCBank(m, 0);

    // Bank 3 (RAM): plain read/write.
    m.memory().write(0xB003, 0x00); // pin-18 strobe -> bank 3
    m.memory().write(0x8000, 0x5A);
    CHECK(m.memory().read(0x8000) == 0x5A);

    // Bank 10 (FLASH): a bare store does nothing.
    m.memory().write(0xB00A, 0x00); // strobe -> bank 0x0A
    const uint8_t before = m.memory().read(0x8000);
    m.memory().write(0x8000, 0x11);
    CHECK(m.memory().read(0x8000) == before); // no unlock -> array untouched

    // JEDEC unlock + byte-program, addressed through the slot window.
    m.memory().write(0x9555, 0xAA); // (0x555, 0xAA)
    m.memory().write(0xAAAA, 0x55); // (0x2AA, 0x55)
    m.memory().write(0x9555, 0xA0); // (0x555, 0xA0) -> program armed
    m.memory().write(0x8010, 0x00); // program &0010 in bank 0x0A: anything & 0x00
    CHECK(m.memory().read(0x8010) == 0x00);
    CHECK(m.memory().read(0x8000) == before); // the neighbouring byte is untouched

    // Host poke() bypasses the lock even on a flash bank.
    m.memory().poke(0x8000, 0x33);
    CHECK(m.memory().read(0x8000) == 0x33);
}

// On a real PC-1600 the CE-1638 / CE-163F trigger-latch modules behave as
// a plain unbanked 16K expansion, and they go in Slot 2 -- where pins
// 16-18 are the dormant K0-K2 lines, so the pin-18 bank strobe never fires
// and bank 0 (RAM) is presented across the whole &8000-&BFFF window. The
// boot ROM must credit that as +16384: BASIC RAM base drops C0C5H -> 80C5H
// (stock - 0x4000), every RAM-base/size work-area pointer moving by 16384.
// (In Slot 1 the same card gets nothing, because S3 on pin 18 trips the
// bank latch throughout the boot RAM-sizing scan.)
void test_trigger_latch_modules_in_slot2_contribute_full_16k() {
    auto boot = [](std::unique_ptr<ExpansionCard> card) -> std::unique_ptr<PC1600Machine> {
        auto m = std::make_unique<PC1600Machine>();
        if (!loadPC1600Roms(*m)) return nullptr;
        if (card) m->attachSlot2Card(std::move(card));
        m->allReset();
        m->runCycles(PC1600Machine::kTStateHz * 4);
        return m;
    };
    auto rd16 = [](PC1600Machine& m, uint16_t a) {
        return uint16_t(m.memory().read(a) | (m.memory().read(uint16_t(a + 1)) << 8));
    };
    auto m0 = boot(nullptr);
    auto c1 = card("ce1638.card.yaml", CardHost::PC1600Slot2);
    auto c2 = card("ce163f.card.yaml", CardHost::PC1600Slot2);
    if (!c1 || !c2) return;
    auto m1 = boot(std::move(c1));
    auto m2 = boot(std::move(c2));
    if (!m0 || !m1 || !m2) {
        std::fprintf(stderr, "SKIP test_trigger_latch_modules_in_slot2_contribute_full_16k: PC-1600 ROM images not found\n");
        return;
    }
    CHECK(rd16(*m0, 0xF5CF) == 0xC0C5);
    CHECK(rd16(*m1, 0xF5CF) == 0x80C5); // CE-1638:  stock - 16384
    CHECK(rd16(*m2, 0xF5CF) == 0x80C5); // CE-163F:  stock - 16384
    CHECK(int(rd16(*m0, 0xF89D)) - int(rd16(*m1, 0xF89D)) == 16384);
    CHECK(int(rd16(*m0, 0xF89D)) - int(rd16(*m2, 0xF89D)) == 16384);
}

} // namespace

int run_pc1600_slot_module_tests() {
    test_ce155_in_slot1_both_cpu_views();
    test_plain_ram_card_via_attach_slot_card();
    test_poke_memory_uses_the_host_path_and_verifies();
    test_slot2_plain_card_ignores_vertical_bank_through_connector();
    test_ce1638_in_slot1_banked_window();
    test_ce163f_in_slot1_ram_and_flash_protocol();
    test_trigger_latch_modules_in_slot2_contribute_full_16k();
    test_boot_with_ce155_in_slot1_is_stable();
    test_ce155_contributes_full_8k_to_mem();

    std::printf("pc1600_slot_module_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
