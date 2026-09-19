// Headless C++ tests for CE1600FCard -- the CE-1600F floppy drive's
// register-level protocol, confirmed by direct disassembly of the real
// roms/PC1600-P1-B5-CE1600P-OR-F.bin (see CE1600FCard.hpp's class comment
// for the file-offset citations). Same no-framework, assert-and-tally
// style as lh5801_tests.cpp -- see that file's header comment.
//
// Build & run: see tools/run_tests.sh

#include <cstdio>
#include <vector>

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
    const auto image = card.imageForSave();
    CHECK(image.size() == CE1600FCard::kImageSize);
    bool allZero = true;
    for (uint8_t b : image) if (b != 0) { allZero = false; break; }
    CHECK(allZero);
}

// Motor-start + status poll, mirroring the ROM's own sequence at bank-5
// file offset 0x413 (write 0x81 to base+2) / 0x480 (poll base+0 bit7).
// The Service Manual's own "Motor startup time: 0.5 second" spec is
// modeled as a real timed busy window (advance()), not instant completion.
void test_motor_start_clears_engine_not_started_bit() {
    CE1600FCard card;
    uint8_t status = readReg(card, 0x78);
    CHECK((status & 0x80) != 0);  // engine not started before motor-on
    writeReg(card, 0x7A, 0x81);   // base+2: bit7 = motor on
    status = readReg(card, 0x78);
    CHECK((status & 0x80) != 0);  // still spinning up -- not instant
    card.advance(CE1600FCard::kMotorStartupTStates);
    status = readReg(card, 0x78);
    CHECK((status & 0x80) == 0);  // engine started once the real 0.5s elapses
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

// Seeks to `track` the way the ROM does at bank-5 0x4933-0x494c: target
// in DATA, then command 0x20, then waits out the mechanical busy window.
void seekTo(CE1600FCard& card, uint8_t track) {
    writeReg(card, 0x7B, track);
    writeReg(card, 0x78, 0x20);
    card.advance(CE1600FCard::kSettleTStates + CE1600FCard::kTracksPerSide * CE1600FCard::kStepTStates);
}

// Format track (0xA0), as at bank-5 0x4555-0x4586: eight 3-byte ID fields
// (track, sector, 1) through DATA, then the track's data is laid down --
// the current head track only, leaving the rest of the disk alone.
void test_format_track_takes_eight_id_fields_and_clears_the_head_track() {
    CE1600FCard card;
    std::vector<uint8_t> image(CE1600FCard::kImageSize, 0x77);
    CHECK(card.loadImage(image.data(), image.size()));
    seekTo(card, 2);

    writeReg(card, 0x78, 0xA0);
    for (uint8_t sector = 0; sector < 8; ++sector) {
        CHECK((readReg(card, 0x7A) & 0x83) == 0x83);  // busy + data request
        writeReg(card, 0x7B, 2);
        writeReg(card, 0x7B, sector);
        writeReg(card, 0x7B, 1);
    }
    CHECK((readReg(card, 0x7A) & 0x83) == 0);  // done after the last ID byte

    const auto after = card.imageForSave();
    const size_t trackBytes = CE1600FCard::kSectorsPerTrack * CE1600FCard::kSectorSize;
    CHECK(after[2 * trackBytes] == 0);
    CHECK(after[3 * trackBytes - 1] == 0);
    CHECK(after[2 * trackBytes - 1] == 0x77);  // track 1 untouched
    CHECK(after[3 * trackBytes] == 0x77);      // track 3 untouched
}

// Seek (0x20, target in DATA) moves the head; read ID (0x80) then returns
// the ID field (track, sector, size code) -- the seek-verify at bank-5
// 0x4976-0x49a9 compares its first byte against the seek target. Busy
// stays set while the three bytes are read and drops right after.
void test_seek_then_read_id_returns_the_target_track() {
    CE1600FCard card;
    seekTo(card, 5);
    CHECK(card.track() == 5);
    CHECK((readReg(card, 0x7A) & 0x81) == 0);

    writeReg(card, 0x78, 0x80);
    CHECK((readReg(card, 0x7A) & 0x83) == 0x83);
    CHECK(readReg(card, 0x7B) == 5);
    CHECK((readReg(card, 0x7A) & 0x83) == 0x83);
    CHECK(readReg(card, 0x7B) < CE1600FCard::kSectorsPerTrack);
    CHECK((readReg(card, 0x7A) & 0x83) == 0x83);
    CHECK(readReg(card, 0x7B) == 1);
    CHECK((readReg(card, 0x7A) & 0xDF) == 0);  // idle, no error bits

    writeReg(card, 0x78, 0x01);  // restore
    card.advance(CE1600FCard::kSettleTStates + 5 * CE1600FCard::kStepTStates);
    CHECK(card.track() == 0);
    CHECK((readReg(card, 0x7A) & 0x81) == 0);
}

// The sector register selects 0-7 within the head's current track, so the
// same sector number on different tracks addresses different image bytes.
void test_sector_address_combines_head_track_and_sector_register() {
    CE1600FCard card;
    seekTo(card, 3);
    writeReg(card, 0x79, 4);
    writeReg(card, 0x78, 0x60);
    writeReg(card, 0x7B, 0xAB);
    const auto image = card.imageForSave();
    CHECK(image[(3 * CE1600FCard::kSectorsPerTrack + 4) * CE1600FCard::kSectorSize] == 0xAB);
    CHECK(image[4 * CE1600FCard::kSectorSize] == 0);
}

// Outside a write transfer, DATA is only a latch (the seek target) -- it
// must never land in the disk image.
void test_data_writes_outside_a_transfer_do_not_touch_the_image() {
    CE1600FCard card;
    const uint64_t revBefore = card.revision();
    writeReg(card, 0x7B, 0x09);
    CHECK(card.revision() == revBefore);
    CHECK(card.imageForSave()[0] == 0);
    CHECK(readReg(card, 0x7B) == 0x09);
}

// A transfer the ROM abandons part-way (the verify bail-out at bank-5
// 0x4909) still completes on its own once the sector has passed the head.
void test_abandoned_transfer_times_out() {
    CE1600FCard card;
    writeReg(card, 0x79, 0);
    writeReg(card, 0x78, 0x40);
    readReg(card, 0x7B);
    CHECK((readReg(card, 0x7A) & 0x81) == 0x81);
    card.advance(CE1600FCard::kTransferIdleTStates);
    CHECK((readReg(card, 0x7A) & 0x83) == 0);
}

// Port 0x81 bit0 write (active-low) resets latched command/motor state --
// confirmed at bank-5 file offset 0xf7 (`xor a` / `out (081h),a`). This
// must not collide with CE1600PCard's own (read-only) use of port 0x81.
void test_port_0x81_reset_clears_motor_and_command_state() {
    CE1600FCard card;
    writeReg(card, 0x7A, 0x81);  // motor on
    card.advance(CE1600FCard::kMotorStartupTStates);
    CHECK((readReg(card, 0x78) & 0x80) == 0);

    CHECK(card.respondsToWrite(ioPins(0x81, true), 0x00));  // bit0=0: reset asserted
    CHECK((readReg(card, 0x78) & 0x80) != 0);  // engine not started again
}

// loadImage()/insertBlankDisk() bump the revision -- the change counter
// FloppyDiskManager polls on each frame tick to decide on autosave.
void test_load_image_and_insert_blank_bump_revision() {
    CE1600FCard card;
    std::vector<uint8_t> image(CE1600FCard::kImageSize, 0x77);
    uint64_t revBefore = card.revision();
    CHECK(card.loadImage(image.data(), image.size()));
    CHECK(card.revision() > revBefore);
    CHECK(card.imageForSave()[0] == 0x77);

    revBefore = card.revision();
    card.insertBlankDisk();
    CHECK(card.revision() > revBefore);
    CHECK(card.imageForSave()[0] == 0);

    CHECK(!card.loadImage(image.data(), image.size() - 1));  // wrong size rejected
}

// A freshly constructed/blanked/loaded card reports "disk changed" (base+2
// bit6) until a seek command (0x20) acknowledges it -- modeled on a real
// FDC's DSKCHG latch.
void test_disk_changed_latch_starts_set_and_clears_on_step() {
    CE1600FCard card;
    CHECK((readReg(card, 0x7A) & 0x40) != 0);
    writeReg(card, 0x78, 0x20);  // seek command
    CHECK((readReg(card, 0x7A) & 0x40) == 0);

    card.insertBlankDisk();
    CHECK((readReg(card, 0x7A) & 0x40) != 0);
}

// Confirmed via register-level tracing against the real ROM: DSKINIT's
// very first command to a freshly-changed drive is 0x01, not 0x20, and it
// immediately polls base+2 -- if the changed-disk latch only cleared on
// 0x20 (an earlier draft's unconfirmed guess), that poll would still see
// it set and DSKINIT would abort. The latch must clear on any command.
void test_disk_changed_latch_clears_on_any_command_not_just_step() {
    CE1600FCard card;
    CHECK((readReg(card, 0x7A) & 0x40) != 0);
    writeReg(card, 0x78, 0x01);  // DSKINIT's first command, not a step
    CHECK((readReg(card, 0x7A) & 0x40) == 0);
}

// setSide() is the software analogue of ejecting and flipping the disk --
// it re-arms the changed-disk latch and switches which 64KB half of the
// image sector/data access addresses, without disturbing the other side's
// contents.
void test_set_side_switches_data_and_rearms_changed_latch() {
    CE1600FCard card;
    writeReg(card, 0x78, 0x20);  // acknowledge the initial changed-disk latch
    CHECK((readReg(card, 0x7A) & 0x40) == 0);
    CHECK(card.side() == 0);

    writeReg(card, 0x79, 0);
    writeReg(card, 0x78, 0x60);
    writeReg(card, 0x7B, 0xAA);  // side A, sector 0, byte 0 = 0xAA

    card.setSide(1);
    CHECK(card.side() == 1);
    CHECK((readReg(card, 0x7A) & 0x40) != 0);  // flipping the disk re-arms it
    seekTo(card, 0);                           // acknowledge again (DATA still holds 0xAA)

    writeReg(card, 0x79, 0);
    writeReg(card, 0x78, 0x60);
    writeReg(card, 0x7B, 0xBB);  // side B, sector 0, byte 0 = 0xBB

    const auto image = card.imageForSave();
    CHECK(image.size() == CE1600FCard::kImageSize);
    CHECK(image[0] == 0xAA);                          // side A untouched by the side-B write
    CHECK(image[CE1600FCard::kSideSize] == 0xBB);      // side B lives right after side A

    card.setSide(0);
    CHECK(card.side() == 0);
    writeReg(card, 0x79, 0);
    writeReg(card, 0x78, 0x40);
    CHECK(readReg(card, 0x7B) == 0xAA);  // back on side A, original byte intact
}

// A transfer (here: read sector) holds busy for exactly as long as its
// bytes are being moved -- the ROM's transfer loops (bank-5 0x4841) treat
// busy dropping mid-transfer as an error -- and releases it right after
// the last byte, well inside the ~9ms `sub_44e4` b=1 poll that follows.
void test_transfer_holds_busy_until_the_last_byte() {
    CE1600FCard card;
    writeReg(card, 0x79, 0);
    writeReg(card, 0x78, 0x40);
    for (int i = 0; i < 512; ++i) {
        CHECK((readReg(card, 0x7A) & 0x83) == 0x83);
        readReg(card, 0x7B);
        card.advance(100);  // a slow poll loop must not time the transfer out
    }
    CHECK((readReg(card, 0x7A) & 0x83) == 0);
}

// A seek busies the drive for the real mechanical time (Service Manual
// §1: 80ms per track + 50ms settling) before base+2 bit0/bit7 clear.
void test_seek_busies_the_drive_for_the_real_step_time() {
    CE1600FCard card;
    writeReg(card, 0x7B, 4);
    writeReg(card, 0x78, 0x20);
    CHECK((readReg(card, 0x7A) & 0x81) == 0x81);
    card.advance(CE1600FCard::kSettleTStates + 3 * CE1600FCard::kStepTStates);
    CHECK((readReg(card, 0x7A) & 0x81) == 0x81);  // four tracks, not three
    card.advance(CE1600FCard::kStepTStates);
    CHECK((readReg(card, 0x7A) & 0x81) == 0);
}

// motorOn() is the "green lamp" the GUI reads to tell the user when it's
// safe to eject and flip the disk.
void test_motor_on_reflects_motor_register_writes() {
    CE1600FCard card;
    CHECK(!card.motorOn());
    writeReg(card, 0x7A, 0x81);
    CHECK(card.motorOn());
    writeReg(card, 0x7A, 0x00);
    CHECK(!card.motorOn());
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
    test_format_track_takes_eight_id_fields_and_clears_the_head_track();
    test_seek_then_read_id_returns_the_target_track();
    test_sector_address_combines_head_track_and_sector_register();
    test_data_writes_outside_a_transfer_do_not_touch_the_image();
    test_abandoned_transfer_times_out();
    test_port_0x81_reset_clears_motor_and_command_state();
    test_load_image_and_insert_blank_bump_revision();
    test_disk_changed_latch_starts_set_and_clears_on_step();
    test_disk_changed_latch_clears_on_any_command_not_just_step();
    test_set_side_switches_data_and_rearms_changed_latch();
    test_motor_on_reflects_motor_register_writes();
    test_transfer_holds_busy_until_the_last_byte();
    test_seek_busies_the_drive_for_the_real_step_time();
    test_claims_only_its_own_ports();

    std::printf("ce1600f_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
