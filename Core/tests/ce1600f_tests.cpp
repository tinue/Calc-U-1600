// Headless C++ tests for CE1600FCard -- the CE-1600F floppy drive's
// register-level protocol, confirmed by direct disassembly of the real
// roms/PC1600-P1-B5-CE1600P-OR-F.bin (see CE1600FCard.hpp's class comment
// for the file-offset citations). Same no-framework, assert-and-tally
// style as lh5801_tests.cpp -- see that file's header comment.
//
// Build & run: see tools/run_tests.sh

#include <cstdio>

#include "../Connector/CE1600FCard.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

PC1600BusPins ioPins(uint8_t port, bool forWrite) {
    PC1600BusPins pins;
    pins.address = port;
    pins.io = true;
    pins.forWrite = forWrite;
    return pins;
}

uint8_t readReg(CE1600FCard& card, uint8_t port) {
    uint8_t v = 0;
    CHECK(card.respondsToRead(ioPins(port, false), v));
    return v;
}

void writeReg(CE1600FCard& card, uint8_t port, uint8_t value) {
    CHECK(card.respondsToWrite(ioPins(port, true), value));
}

// A freshly constructed card auto-inserts a blank (zero-filled) disk --
// the "always a blank disk if nothing else is specified" requirement.
void test_default_construction_is_blank_disk() {
    CE1600FCard card;
    CHECK(!card.isDirty());
    const auto image = card.imageForSave();
    CHECK(image.size() == CE1600FCard::kImageSize);
    bool allZero = true;
    for (uint8_t b : image) if (b != 0) { allZero = false; break; }
    CHECK(allZero);
}

// Motor-start + status poll, mirroring the ROM's own sequence at bank-5
// file offset 0x413 (write 0x81 to base+2) / 0x480 (poll base+0 bit7).
void test_motor_start_clears_engine_not_started_bit() {
    CE1600FCard card;
    uint8_t status = readReg(card, 0x78);
    CHECK((status & 0x80) != 0);  // engine not started before motor-on
    writeReg(card, 0x7A, 0x81);   // base+2: bit7 = motor on
    status = readReg(card, 0x78);
    CHECK((status & 0x80) == 0);  // engine started
}

// Write a full sector via the command(0x60)/sector(0x79)/data(0x7B)
// registers, then read it back via command(0x40) -- confirms the
// base+0/+1/+3 addressing and the auto-incrementing byte offset.
void test_write_then_read_sector_round_trip() {
    CE1600FCard card;
    const uint64_t revBefore = card.revision();

    writeReg(card, 0x79, 3);     // select sector 3
    writeReg(card, 0x78, 0x60);  // command: write sector
    for (int i = 0; i < 512; ++i) writeReg(card, 0x7B, static_cast<uint8_t>(i & 0xFF));
    CHECK(card.isDirty());
    CHECK(card.revision() > revBefore);

    writeReg(card, 0x79, 3);
    writeReg(card, 0x78, 0x40);  // command: read sector
    bool matches = true;
    for (int i = 0; i < 512; ++i) {
        if (readReg(card, 0x7B) != static_cast<uint8_t>(i & 0xFF)) { matches = false; break; }
    }
    CHECK(matches);

    const auto image = card.imageForSave();
    CHECK(image[3 * CE1600FCard::kSectorSize] == 0);
    CHECK(image[3 * CE1600FCard::kSectorSize + 255] == 255);
}

// Format (0xA0) zero-fills just the selected sector, leaving others alone.
void test_format_command_zero_fills_selected_sector() {
    CE1600FCard card;
    writeReg(card, 0x79, 5);
    writeReg(card, 0x78, 0x60);
    writeReg(card, 0x7B, 0xAA);  // dirty one byte of sector 5

    writeReg(card, 0x79, 5);
    writeReg(card, 0x78, 0xA0);  // format sector 5
    const auto image = card.imageForSave();
    CHECK(image[5 * CE1600FCard::kSectorSize] == 0);
}

// Port 0x81 bit0 write (active-low) resets latched command/motor state --
// confirmed at bank-5 file offset 0xf7 (`xor a` / `out (081h),a`). This
// must not collide with CE1600PCard's own (read-only) use of port 0x81.
void test_port_0x81_reset_clears_motor_and_command_state() {
    CE1600FCard card;
    writeReg(card, 0x7A, 0x81);  // motor on
    CHECK((readReg(card, 0x78) & 0x80) == 0);

    CHECK(card.respondsToWrite(ioPins(0x81, true), 0x00));  // bit0=0: reset asserted
    CHECK((readReg(card, 0x78) & 0x80) != 0);  // engine not started again
}

// loadImage()/insertBlankDisk() clear the dirty flag and bump the
// revision -- the dirty/autosave-eligible contract FloppyDiskManager will
// poll on each frame tick.
void test_load_image_and_insert_blank_manage_dirty_and_revision() {
    CE1600FCard card;
    writeReg(card, 0x79, 0);
    writeReg(card, 0x78, 0x60);
    writeReg(card, 0x7B, 0x42);
    CHECK(card.isDirty());

    std::vector<uint8_t> image(CE1600FCard::kImageSize, 0x77);
    const uint64_t revBefore = card.revision();
    CHECK(card.loadImage(image.data(), image.size()));
    CHECK(!card.isDirty());
    CHECK(card.revision() > revBefore);
    CHECK(card.imageForSave()[0] == 0x77);

    card.insertBlankDisk();
    CHECK(!card.isDirty());
    CHECK(card.imageForSave()[0] == 0);

    CHECK(!card.loadImage(image.data(), image.size() - 1));  // wrong size rejected
}

// Only ports 0x70-0x7F and the write side of 0x81 are claimed -- everything
// else (e.g. CE1600PCard's own 0x81 read, 0x82/0x83) must be left alone.
void test_claims_only_its_own_ports() {
    CE1600FCard card;
    uint8_t v = 0;
    PC1600BusPins romPins;  // io=false: ROM window, not this card's concern
    CHECK(!card.respondsToRead(romPins, v));
    CHECK(!card.respondsToRead(ioPins(0x81, false), v));  // read side is CE1600PCard's
    CHECK(!card.respondsToWrite(ioPins(0x82, true), 0));
    CHECK(!card.respondsToRead(ioPins(0x90, false), v));
}

}  // namespace

int run_ce1600f_tests() {
    test_default_construction_is_blank_disk();
    test_motor_start_clears_engine_not_started_bit();
    test_write_then_read_sector_round_trip();
    test_format_command_zero_fills_selected_sector();
    test_port_0x81_reset_clears_motor_and_command_state();
    test_load_image_and_insert_blank_manage_dirty_and_revision();
    test_claims_only_its_own_ports();

    std::printf("ce1600f_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
