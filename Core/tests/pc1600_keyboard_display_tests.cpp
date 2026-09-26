// Headless C++ tests for the PC1600Keyboard/PC1600Display
// classes and their wiring into PC1600Memory's I/O decode (ports
// 1BH-1FH/37H/50H-5BH). Same no-framework, assert-and-tally style as
// lh5801_tests.cpp -- see that file's header comment.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

#include "../PC1600/PC1600Bank.hpp"
#include "../PC1600/PC1600Display.hpp"
#include "../PC1600/PC1600Keyboard.hpp"
#include "../PC1600/PC1600Memory.hpp"
#include "../PC1600/PC1600StatusLine.hpp"
#include "../SharpShiftedSymbols.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

// ── PC1600Keyboard ────────────────────────────────────────────────────

void test_keyboard_name_lookup() {
    using K = PC1600Keyboard::Key;
    CHECK(PC1600Keyboard::keyFromName("5") == K::Digit5);
    CHECK(PC1600Keyboard::keyFromName("a") == K::A);
    CHECK(PC1600Keyboard::keyFromName("Z") == K::Z);
    CHECK(PC1600Keyboard::keyFromName("enter") == K::Enter);
    CHECK(PC1600Keyboard::keyFromName("ent") == K::Enter);
    CHECK(PC1600Keyboard::keyFromName("+") == K::Plus);
    CHECK(PC1600Keyboard::keyFromName("kbii") == K::Kbii);
    CHECK(PC1600Keyboard::keyFromName("rsv") == K::Rsv);
    CHECK(PC1600Keyboard::keyFromName("nonsense") == K::Unknown);
}

void test_keyboard_rsv_matrix_position() {
    // KS1 bit7 -- see PC1600Keyboard::Key::Rsv's own doc comment for how
    // this was confirmed (ROM key-code table + schematic, 2026-08-30).
    PC1600Keyboard kb;
    kb.setKeyState(PC1600Keyboard::Key::Rsv, true);
    uint8_t sense = kb.scan(0xFD, false); // strobe only KS1 (bit1 low)
    CHECK((sense & 0x80) == 0);
}

void test_keyboard_scan_single_strobe() {
    PC1600Keyboard kb;
    // '5' is KS0 bit1 (PC-1600-Keyboard.md §5).
    kb.setKeyState(PC1600Keyboard::Key::Digit5, true);
    // Strobe only KS0 (bit0 low -> active), rest high (inactive).
    uint8_t sense = kb.scan(0xFE, false);
    CHECK((sense & 0x02) == 0); // bit1 pulled low
    CHECK((sense & 0x01) != 0); // bit0 untouched
    // Strobing a different line (KS1) shouldn't see it.
    uint8_t senseKS1 = kb.scan(0xFD, false);
    CHECK(senseKS1 == 0xFF);
}

void test_keyboard_pb6_strobe() {
    PC1600Keyboard kb;
    kb.setKeyState(PC1600Keyboard::Key::Ctrl, true); // PB6 strobe, bit0
    uint8_t senseNoStrobe = kb.scan(0xFF, false);
    CHECK(senseNoStrobe == 0xFF);
    uint8_t senseWithStrobe = kb.scan(0xFF, true);
    CHECK((senseWithStrobe & 0x01) == 0);
}

void test_keyboard_no_keys_pressed_is_all_ones() {
    PC1600Keyboard kb;
    CHECK(kb.scan(0x00, true) == 0xFF);
}

// ── PC1600Display ─────────────────────────────────────────────────────

void test_display_ic2_command_and_data() {
    PC1600Display d;
    // Port block 0x58-0x5B = IC2-only. Within the block: offset 0 (0x58) =
    // command write, offset 2 (0x5A) = data write, offset 1 (0x59) = status
    // read, offset 3 (0x5B) = data read, per the HD61102 controller's
    // register decode (see the .cpp's own comment).
    d.writeIO(0x58, 0x3F); // display on (command; bit0=1 per the real HD61102 encoding)
    d.writeIO(0x58, 0xBA); // set page 2 (0xB8 | page)
    d.writeIO(0x58, 0x45); // set column 5
    d.writeIO(0x5A, 0x81); // data write: byte 0x81 -> bits 0 and 7 set
    CHECK(d.pixel(5, 2 * 8 + 0)); // page 2, bit0 -> y = 16
    CHECK(d.pixel(5, 2 * 8 + 7)); // bit7 -> y = 23
    CHECK(!d.pixel(5, 2 * 8 + 1));
}

// Status bit 7 (busy) after a write: set until the 4th edge of the
// controllers' 216.7 kHz clock (~16.5 T per edge), then clear. Only the
// written controller goes busy; 50H-53H writes hit both.
void test_display_busy_after_write() {
    PC1600Display d;
    d.setClockEnabled(true);
    d.writeIO(0x50, 0x3F);               // both on, so the status byte is only busy
    d.tick(70);
    CHECK(d.readIO(0x59) == 0x00);
    CHECK(d.readIO(0x55) == 0x00);
    d.writeIO(0x5A, 0x81);               // IC2 data write
    CHECK((d.readIO(0x59) & 0x80) != 0);
    CHECK((d.readIO(0x55) & 0x80) == 0); // IC3 untouched
    d.tick(49);                          // under 3 edges: still busy
    CHECK((d.readIO(0x59) & 0x80) != 0);
    d.tick(18);                          // past the 4th edge (<= 66 T total)
    CHECK((d.readIO(0x59) & 0x80) == 0);
    d.writeIO(0x50, 0x3F);               // both controllers
    CHECK((d.readIO(0x59) & 0x80) != 0);
    CHECK((d.readIO(0x55) & 0x80) != 0);
    d.tick(70);
    CHECK(d.readIO(0x59) == 0x00);
    CHECK(d.readIO(0x55) == 0x00);
}

// Without CK0 (port 37H bit 4) the HD61102s get no phi clock, so busy
// cannot clear; it clears 4 edges after the clock comes back.
void test_display_busy_holds_while_clock_off() {
    PC1600Display d;
    d.writeIO(0x5A, 0x81);
    d.tick(10'000);
    CHECK((d.readIO(0x59) & 0x80) != 0);
    d.setClockEnabled(true);
    d.tick(49);
    CHECK((d.readIO(0x59) & 0x80) != 0);
    d.tick(18);
    CHECK((d.readIO(0x59) & 0x80) == 0);
}

// Status DB5 = ON/OFF, 1 = display off (the reverse of the instruction's
// D bit); DB4 (RESET) stays clear.
void test_display_status_reports_on_off() {
    PC1600Display d;
    d.setClockEnabled(true);
    CHECK(d.readIO(0x59) == 0x20);      // off after power-on
    CHECK(d.readIO(0x55) == 0x20);
    d.writeIO(0x58, 0x3F);              // IC2 on
    d.tick(70);
    CHECK(d.readIO(0x59) == 0x00);
    CHECK(d.readIO(0x55) == 0x20);      // IC3 still off
    d.writeIO(0x58, 0x3E);
    d.tick(70);
    CHECK(d.readIO(0x59) == 0x20);
}

void test_display_ic3_column_offset() {
    PC1600Display d;
    // Port block 0x54-0x57 = IC3-only, columns 64-127 of the panel.
    d.writeIO(0x54, 0x3F);
    d.writeIO(0x54, 0xB8); // page 0 (0xB8 | 0)
    d.writeIO(0x54, 0x40); // column 0 (of IC3 -> panel column 64)
    d.writeIO(0x56, 0x01); // data write: bit0 set -> y=0
    CHECK(d.pixel(64, 0));
    CHECK(!d.pixel(0, 0)); // IC2 untouched
}

void test_display_read_returns_output_register() {
    // See PC1600Display.cpp's dataByte() comment: a read returns the
    // output register, which the previous read loaded, then reloads it
    // from the current address. The first read after setting an address
    // is the dummy read the ROM discards (bank 6 81E8H).
    PC1600Display d;
    d.writeIO(0x58, 0x3F);       // display on
    d.writeIO(0x58, 0xB8);       // page 0
    d.writeIO(0x58, 0x40);       // column 0
    d.writeIO(0x5A, 0x11);       // write column 0 = 0x11, pointer -> column 1
    d.writeIO(0x5A, 0x22);       // write column 1 = 0x22, pointer -> column 2
    d.writeIO(0x58, 0x40);       // re-home: set column 0
    CHECK(d.readIO(0x5B) == 0x00); // dummy: output register still empty
    CHECK(d.readIO(0x5B) == 0x11); // column 0
    CHECK(d.readIO(0x5B) == 0x22); // column 1

    // A write between reads does not touch the output register: the next
    // read still returns what the previous read latched (column 2 = 0).
    d.writeIO(0x58, 0x42);       // column 2
    (void)d.readIO(0x5B);        // dummy, latches column 2 (0x00)
    d.writeIO(0x58, 0x42);
    d.writeIO(0x5A, 0x33);       // column 2 = 0x33
    CHECK(d.readIO(0x5B) == 0x00); // stale latch, not 0x33

    // A page change is the same: the latch keeps the old page's byte.
    d.writeIO(0x58, 0x40);
    (void)d.readIO(0x5B);        // latches page 0 column 0 (0x11)
    d.writeIO(0x58, 0xB9);       // page 1
    CHECK(d.readIO(0x5B) == 0x11);
}

/// A round-trip read-modify-write, exactly the shape of the ROM's own
/// cursor-blink routine (set column, dummy read, real read, XOR a
/// pattern in, re-home the column, write back) -- confirms it recovers
/// the ORIGINAL byte, not a neighboring column's.
void test_display_cursor_style_read_modify_write_roundtrip() {
    PC1600Display d;
    d.writeIO(0x58, 0x3F);
    d.writeIO(0x58, 0xB8); // page 0
    d.writeIO(0x58, 0x45); // column 5
    d.writeIO(0x5A, 0x3C); // seed column 5's content

    d.writeIO(0x58, 0x45);       // set column 5 again (as the ROM would before reading it back)
    (void)d.readIO(0x5B);        // dummy read (stale, discarded by real firmware)
    uint8_t original = d.readIO(0x5B); // real read: column 5's actual byte
    CHECK(original == 0x3C);
    uint8_t toggled = original ^ 0xFF; // XOR the cursor pattern in
    d.writeIO(0x58, 0x45);       // re-home to column 5 before writing back
    d.writeIO(0x5A, toggled);
    // Blink again: toggling back should restore the original byte exactly.
    d.writeIO(0x58, 0x45);
    (void)d.readIO(0x5B);
    uint8_t afterFirstBlink = d.readIO(0x5B);
    CHECK(afterFirstBlink == toggled);
    uint8_t restored = afterFirstBlink ^ 0xFF;
    CHECK(restored == original); // exact round-trip, no drift into a neighboring column
    d.writeIO(0x58, 0x45);
    d.writeIO(0x5A, restored);
}

void test_display_off_reads_as_blank() {
    PC1600Display d;
    d.writeIO(0x58, 0x3F); // on
    d.writeIO(0x58, 0xB8); // page 0
    d.writeIO(0x58, 0x40);
    d.writeIO(0x5A, 0xFF);
    CHECK(d.pixel(0, 0));
    d.writeIO(0x58, 0x3E); // off
    CHECK(!d.pixel(0, 0)); // displayOn gates pixel(), even though the byte is still stored
}

// Columns 128-155 read IC2's own pages 4-7 (columns 0-27 of that same
// chip) instead of pages 0-3 -- genuinely independent, individually-
// addressable pixels, not a duplicate of columns 0-27's own page 0-3
// content. See PC1600Display.hpp's class comment for how this mapping
// was derived from the TRM block diagram.
void test_display_rightmost_columns_read_ic2_pages4to7() {
    PC1600Display d;
    d.writeIO(0x58, 0x3F); // IC2 on

    // Write page 0 (left block), column 0: should NOT appear at column 128.
    d.writeIO(0x58, 0xB8);       // page 0
    d.writeIO(0x58, 0x40);       // column 0
    d.writeIO(0x5A, 0x01);       // data write: bit0 set -> y=0 lit
    CHECK(d.pixel(0, 0));
    CHECK(!d.pixel(128, 0)); // page 0 doesn't reach the right block

    // Write page 4 (right block's own page range), column 0: appears only
    // at column 128 (y=0, the low bit of page 4), not at column 0.
    d.writeIO(0x58, 0xB8 | 4);   // page 4
    d.writeIO(0x58, 0x40);       // column 0
    d.writeIO(0x5A, 0x01);
    CHECK(d.pixel(128, 0));
    CHECK(!d.pixel(1, 0));

    // Write page 7 (the last of the right block's 4 pages, local row 0 of
    // that page -> right-block y = (7-4)*8+0 = 24), column 27: appears at
    // the last right-block column (155), row 24 -- and NOT at the left
    // block's own column 27 (which reads pages 0-3, not page 7),
    // confirming the two blocks are genuinely independent.
    d.writeIO(0x58, 0xB8 | 7);  // page 7
    d.writeIO(0x58, 0x40 | 27); // column 27
    d.writeIO(0x5A, 0x01);
    CHECK(d.pixel(155, 24));
    CHECK(!d.pixel(27, 24));
}

// Command 0xC0-0xFF (display start line) rotates which of a controller's
// 8 pages (64 raw rows) appear at visible row 0-31; the ROM's own scroll
// routines depend on this rotation to avoid drawing stray dots or
// misplaced content.
void test_display_start_line_rotates_visible_window() {
    PC1600Display d;
    d.writeIO(0x58, 0x3F); // IC2 on

    // Page 0, column 0, bit 0 set -- visible at y=0 while start line is 0;
    // page 1, column 0 left all-zero for now.
    d.writeIO(0x58, 0xB8);      // page 0
    d.writeIO(0x58, 0x40);      // column 0
    d.writeIO(0x5A, 0x01);
    CHECK(d.pixel(0, 0));

    // Start line = 8 (one page): y=0 now reads raw row 8, i.e. page 1
    // bit 0 -- currently 0 -- NOT page 0's still-set bit 0. Proves this
    // is a genuine rotation to different underlying data, not e.g. a
    // no-op that happens to leave the old bit visibly set.
    d.writeIO(0x58, 0xC0 | 8); // display start line = 8
    CHECK(!d.pixel(0, 0));

    // Now set page 1 bit 0 too: y=0 should read it.
    d.writeIO(0x58, 0xB8 | 1); // page 1
    d.writeIO(0x58, 0x40);     // column 0
    d.writeIO(0x5A, 0x01);
    CHECK(d.pixel(0, 0));
}

// Status symbols are real display memory -- IC3's own column 63, pages
// 7/6/4 -- per the TRM's own SMBLSET table. Writing through the ordinary
// HD61102 I/O path (ports 54H/56H, IC3) should update statusLine()
// automatically.
void test_display_status_symbols_wired_to_ic3_column63() {
    PC1600Display d;
    d.writeIO(0x54, 0x3F); // IC3 on

    // Page 7 (B=00H row): DEF (bit7) + BUSY (bit0) set.
    d.writeIO(0x54, 0xB8 | 7);
    d.writeIO(0x54, 0x40 | 63);
    d.writeIO(0x56, 0x81);

    // Page 6 (B=01H row): RUN (bit6) set.
    d.writeIO(0x54, 0xB8 | 6);
    d.writeIO(0x54, 0x40 | 63);
    d.writeIO(0x56, 0x40);

    // Page 4 (B=02H row): S (bit3) + BATT (bit0) set.
    d.writeIO(0x54, 0xB8 | 4);
    d.writeIO(0x54, 0x40 | 63);
    d.writeIO(0x56, 0x09);

    using Symbol = PC1600StatusLine::Symbol;
    CHECK(d.statusLine().isOn(Symbol::Def));
    CHECK(d.statusLine().isOn(Symbol::Busy));
    CHECK(!d.statusLine().isOn(Symbol::Shift));
    CHECK(!d.statusLine().isOn(Symbol::Small));
    CHECK(!d.statusLine().isOn(Symbol::I));

    CHECK(d.statusLine().isOn(Symbol::Run));
    CHECK(!d.statusLine().isOn(Symbol::Pro));
    CHECK(!d.statusLine().isOn(Symbol::Reserve));
    CHECK(!d.statusLine().isOn(Symbol::Rad));
    CHECK(!d.statusLine().isOn(Symbol::Grad));
    CHECK(!d.statusLine().isOn(Symbol::Deg));

    CHECK(d.statusLine().isOn(Symbol::S));
    CHECK(d.statusLine().isOn(Symbol::Batt));
    CHECK(!d.statusLine().isOn(Symbol::Ctrl));
    CHECK(!d.statusLine().isOn(Symbol::Romaji));
    CHECK(!d.statusLine().isOn(Symbol::Kana));
}

// A "display off" command (0x3E) to IC3 must also blank the status-symbol
// strip -- those segments hang off IC3 (PC-1600-Display-HD61202.md §2), so
// the ROM's OFF-key / auto-power-off power-down should not leave DEG/RUN/
// BUSY frozen on the glass. Turning the display back on recomputes from the
// retained pixel RAM.
void test_display_status_symbols_blank_when_ic3_display_off() {
    PC1600Display d;
    using Symbol = PC1600StatusLine::Symbol;

    d.writeIO(0x54, 0x3F); // IC3 on
    d.writeIO(0x54, 0xB8 | 7);
    d.writeIO(0x54, 0x40 | 63);
    d.writeIO(0x56, 0x81); // DEF (bit7) + BUSY (bit0)
    CHECK(d.statusLine().isOn(Symbol::Def));
    CHECK(d.statusLine().isOn(Symbol::Busy));

    // Power-down: the ROM issues 0x3E to the "both controllers" port.
    d.writeIO(0x50, 0x3E);
    for (bool on : d.statusLine().all()) CHECK(!on);

    // Power back up: segments resume from the retained cell contents.
    d.writeIO(0x50, 0x3F);
    CHECK(d.statusLine().isOn(Symbol::Def));
    CHECK(d.statusLine().isOn(Symbol::Busy));
}

void test_display_clock_enable_flag() {
    PC1600Display d;
    CHECK(!d.clockEnabled());
    d.setClockEnabled(true);
    CHECK(d.clockEnabled());
}

// ── PC1600StatusLine ─────────────────────────────────────────────────

void test_statusline_defaults_all_off() {
    PC1600StatusLine s;
    for (bool on : s.all()) CHECK(!on);
}

void test_statusline_set_and_read() {
    PC1600StatusLine s;
    s.set(PC1600StatusLine::Symbol::Batt, true);
    CHECK(s.isOn(PC1600StatusLine::Symbol::Batt));
    CHECK(!s.isOn(PC1600StatusLine::Symbol::Busy));
    s.set(PC1600StatusLine::Symbol::Batt, false);
    CHECK(!s.isOn(PC1600StatusLine::Symbol::Batt));
}

void test_statusline_reset_clears_all() {
    PC1600StatusLine s;
    s.set(PC1600StatusLine::Symbol::Deg, true);
    s.set(PC1600StatusLine::Symbol::Ctrl, true);
    s.reset();
    for (bool on : s.all()) CHECK(!on);
}

void test_display_owns_status_line() {
    PC1600Display d;
    CHECK(!d.statusLine().isOn(PC1600StatusLine::Symbol::Shift));
    d.statusLine().set(PC1600StatusLine::Symbol::Shift, true);
    CHECK(d.statusLine().isOn(PC1600StatusLine::Symbol::Shift));
}

// ── PC1600Memory I/O wiring ────────────────────────────────────────────

void test_memory_keyboard_and_on_key_via_io() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    mem.keyboard().setKeyState(PC1600Keyboard::Key::Digit1, true); // KS2 bit0
    // Real firmware pattern: OUT (1EH),KS-mask ; IN A,(37H).
    // We drive PC1600Memory's SC7852Bus interface directly since that's
    // what the CPU would use.
    static_cast<SC7852Bus&>(mem).writeIO(0x1E, static_cast<uint8_t>(~(1 << 2))); // strobe only KS2
    uint8_t sense = static_cast<SC7852Bus&>(mem).readIO(0x37);
    CHECK((sense & 0x01) == 0);

    CHECK(static_cast<SC7852Bus&>(mem).readIO(0x1B) == 0x00);
    auto& bus = static_cast<SC7852Bus&>(mem);
    CHECK((bus.readIO(0x1F) & 0x80) == 0);
    CHECK((bus.readIO(0x1A) & 0x20) == 0);
    mem.setOnKeyPressed(true);
    CHECK((bus.readIO(0x1B) & 0x02) != 0);
    // The live level too (Baum Systemhandbuch p.92 / Anhang A): PB7 = &1F bit 7, and
    // the MSK read's PB7 slot = &1A bit 5, both 1 while held.
    CHECK((bus.readIO(0x1F) & 0x80) != 0);
    CHECK((bus.readIO(0x1A) & 0x20) != 0);
    mem.reset(); // a physical input: still held across a reset
    CHECK((bus.readIO(0x1F) & 0x80) != 0);
    mem.setOnKeyPressed(false);
    CHECK((bus.readIO(0x1F) & 0x80) == 0);
    CHECK((bus.readIO(0x1A) & 0x20) == 0);
    CHECK((bus.readIO(0x1B) & 0x02) != 0); // the IF latch stays until the ROM clears it
    // MSK keeps its low nibble; the upper nibble is the live inputs.
    bus.writeIO(0x1A, 0xFF);
    CHECK(bus.readIO(0x1A) == 0x0F);
}

void test_memory_pb6_strobe_needs_output_mode_via_io() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    auto& bus = static_cast<SC7852Bus&>(mem);
    mem.keyboard().setKeyState(PC1600Keyboard::Key::Kbii, true); // PB6 strobe, bit1
    mem.keyboard().setKeyState(PC1600Keyboard::Key::Mode, true); // KS6 bit1 -- shares KIN1 with KBII

    // No KS line strobed. Driving OPB.6 low but leaving PB6 as an input
    // (DDB.6 = 0) must NOT strobe -- the line is pulled up internally.
    bus.writeIO(0x1E, 0xFF);        // no KS strobe
    bus.writeIO(0x1F, ~0x40 & 0xFF); // OPB.6 = 0
    CHECK(bus.readIO(0x37) == 0xFF);

    // Configure PB6 as an output: now OPB.6 = 0 asserts the strobe and KBII
    // (bit1) reads, while MODE (KS6, not strobed) does not leak in.
    bus.writeIO(0x1D, 0x40);        // DDB.6 = 1 (output)
    CHECK((bus.readIO(0x37) & 0x02) == 0); // bit1 low = KBII seen
    CHECK((bus.readIO(0x37) & 0x05) != 0); // CTRL/BS bits untouched

    // Release the strobe the way the service manual documents: OPB.6 high,
    // then flip PB6 back to input. Either step alone must drop the strobe.
    bus.writeIO(0x1F, 0x40);        // OPB.6 = 1
    CHECK(bus.readIO(0x37) == 0xFF);
    bus.writeIO(0x1F, ~0x40 & 0xFF); // OPB.6 = 0 again, but...
    bus.writeIO(0x1D, 0x00);        // ...PB6 now an input -> pulled up
    CHECK(bus.readIO(0x37) == 0xFF);

    // With PB6 released, strobing KS6 reads MODE cleanly on the same bit1.
    bus.writeIO(0x1E, static_cast<uint8_t>(~(1 << 6))); // strobe KS6
    CHECK((bus.readIO(0x37) & 0x02) == 0); // bit1 low = MODE, no KBII collision
}

void test_memory_pb5_survives_opb_read_modify_write() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    auto& bus = static_cast<SC7852Bus&>(mem);

    // PB5 is an input pin (the sub-CPU's 64Hz timer square wave), so it must
    // read back as the live level regardless of what the CPU wrote to OPB.
    mem.setTimer64Bit(true);
    CHECK((bus.readIO(0x1F) & 0x20) != 0);
    mem.setTimer64Bit(false);
    CHECK((bus.readIO(0x1F) & 0x20) == 0);

    // The discriminating case: a *write* must not be able to forge an
    // input pin's level. PB5 is an input (DDB.5 = 0), so writing bit 5 high
    // must not make it read back high -- only setTimer64Bit() drives it.
    mem.setTimer64Bit(false);
    bus.writeIO(0x1D, 0x00);        // whole PB port an input
    bus.writeIO(0x1F, 0xFF);        // try to force every bit high
    CHECK((bus.readIO(0x1F) & 0x20) == 0);
    mem.setTimer64Bit(true);
    CHECK((bus.readIO(0x1F) & 0x20) != 0);
    // ...and once PB5 is an output (hypothetically), the latch wins again.
    bus.writeIO(0x1D, 0x20);
    bus.writeIO(0x1F, 0x00);
    CHECK((bus.readIO(0x1F) & 0x20) == 0);
    bus.writeIO(0x1D, 0x00);

    // Replay the ROM's own key-scan strobe assert/release pair verbatim
    // (PC1600-P1-B3-new.bin 4887H and 4896H): PB5 must stay driven by the timer
    // throughout this exact sequence.
    mem.setTimer64Bit(true);
    // 4887H: IN A,(1DH) / OR 40H / OUT (1DH),A  -- PB6 to output
    bus.writeIO(0x1D, static_cast<uint8_t>(bus.readIO(0x1D) | 0x40));
    // ...then IN A,(1FH) / AND BFH / OUT (1FH),A -- drive PB6 low
    bus.writeIO(0x1F, static_cast<uint8_t>(bus.readIO(0x1F) & 0xBF));
    CHECK((bus.readIO(0x1F) & 0x20) != 0); // PB5 still high
    CHECK((bus.readIO(0x1F) & 0x40) == 0); // PB6 (an output) reads its latch

    // 4896H: IN A,(1FH) / OR 40H / OUT (1FH),A, then DDB.6 back to input.
    bus.writeIO(0x1F, static_cast<uint8_t>(bus.readIO(0x1F) | 0x40));
    bus.writeIO(0x1D, static_cast<uint8_t>(bus.readIO(0x1D) & 0xBF));
    CHECK((bus.readIO(0x1F) & 0x20) != 0); // PB5 survived the whole cycle

    // And the timer still drives it after all that port traffic.
    mem.setTimer64Bit(false);
    CHECK((bus.readIO(0x1F) & 0x20) == 0);
    mem.setTimer64Bit(true);
    CHECK((bus.readIO(0x1F) & 0x20) != 0);
}

// ── PC1600SubCpu (LU-57813P) ──────────────────────────────────────────

// The values below are what the Z-80 writes to port 21H: the complement of
// the sub-CPU operand (PC1600SubCpu's class comment). Operands in brackets.
void test_subcpu_requests_answer_on_port33() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    auto& bus = static_cast<SC7852Bus&>(mem);

    // 5AH [A5H, IOCS 15H]: the reset cause, the first request -> A0H.
    bus.writeIO(0x21, 0x5A);
    CHECK(mem.subCpu().answerPending());
    CHECK(bus.readIO(0x33) == 0xA0);
    CHECK(!mem.subCpu().answerPending());

    // 57H [A8H SRA0, main supply] / 55H [AAH SRA2, CE-1600P pack]: both must clear the ROM's
    // low-battery thresholds (AFH and A8H respectively).
    bus.writeIO(0x21, 0x57);
    CHECK(bus.readIO(0x33) == 0xC0);
    bus.writeIO(0x21, 0x55);
    CHECK(bus.readIO(0x33) == 0xC0);

    // 56H [A9H SRA1] answers the injected analog reading.
    mem.subCpu().setAnalogInput(0x7B);
    bus.writeIO(0x21, 0x56);
    CHECK(bus.readIO(0x33) == 0x7B);

    // 5CH [A3H SRINP]: bit5 = CI not asserted, bit2 = password stored.
    bus.writeIO(0x21, 0x5C);
    CHECK(bus.readIO(0x33) == 0x20);

    // The answer register keeps driving its last value between commands --
    // the ROM reads 33H at points with no preceding command (bank 6,
    // A8F5H), and must not see an open-bus FFH there.
    CHECK(bus.readIO(0x33) == 0x20);
    CHECK(bus.readIO(0x33) == 0x20);
}

void test_subcpu_nibble_stack_clock_roundtrip() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    auto& bus = static_cast<SC7852Bus&>(mem);

    // Set the clock to 09-25 14:37:52 by pushing nine nibbles (each sent
    // as its complement: 0x = F0H+n starts the block, 7x = 80H+n appends)
    // and committing with 6DH [92H SWRT].
    const uint8_t nib[9] = {0x09, 0x2, 0x5, 0x1, 0x4, 0x3, 0x7, 0x5, 0x2};
    bus.writeIO(0x21, static_cast<uint8_t>(0x00 | (0x0F - nib[0])));
    for (int i = 1; i < 9; i++) {
        bus.writeIO(0x21, static_cast<uint8_t>(0x70 | (0x0F - nib[i])));
    }
    bus.writeIO(0x21, 0x6D);
    PC1600SubCpu::DateTime dt = mem.subCpu().dateTime();
    CHECK(dt.month == 0x09);
    CHECK(dt.day == 0x25);
    CHECK(dt.hour == 0x14);
    CHECK(dt.minute == 0x37);
    CHECK(dt.second == 0x52);

    // A field pushed as all-1s nibbles means "leave alone": rewrite only
    // the seconds, keeping everything else.
    const uint8_t keep[9] = {0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0x0, 0x1};
    bus.writeIO(0x21, static_cast<uint8_t>(0x00 | (0x0F - keep[0])));
    for (int i = 1; i < 9; i++) {
        bus.writeIO(0x21, static_cast<uint8_t>(0x70 | (0x0F - keep[i])));
    }
    bus.writeIO(0x21, 0x6D);
    dt = mem.subCpu().dateTime();
    CHECK(dt.month == 0x09);   // untouched
    CHECK(dt.hour == 0x14);    // untouched
    CHECK(dt.second == 0x01);  // rewritten

    // Read the clock back the way BASIC's `TIME` does: 6CH [93H SRRT] publishes
    // the nine nibbles, then one 6FH [90H fetch] per nibble moves each into the
    // answer register for the following IN A,(33H). Expected clock now:
    // month 09, day 25, 14:37:01.
    const uint8_t want[9] = {0x09, 0x2, 0x5, 0x1, 0x4, 0x3, 0x7, 0x0, 0x1};
    bus.writeIO(0x21, 0x6C);
    for (int i = 0; i < 9; i++) {
        bus.writeIO(0x21, 0x6F);
        CHECK(bus.readIO(0x33) == want[i]);
    }
}

void test_subcpu_clock_ticks_and_rolls_over() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    auto& sub = mem.subCpu();

    // Plain second bump, BCD-encoded fields.
    sub.setYear(2025);
    sub.setDateTime({0x09, 0x25, 0x14, 0x37, 0x08});
    sub.tickOneSecond();
    CHECK(sub.dateTime().second == 0x09);
    sub.tickOneSecond(); // 09 -> 10 must land as BCD 0x10, not 0x0A
    CHECK(sub.dateTime().second == 0x10);

    // 23:59:59 -> next day 00:00:00.
    sub.setDateTime({0x09, 0x25, 0x23, 0x59, 0x59});
    sub.tickOneSecond();
    PC1600SubCpu::DateTime dt = sub.dateTime();
    CHECK(dt.second == 0x00 && dt.minute == 0x00 && dt.hour == 0x00);
    CHECK(dt.day == 0x26 && dt.month == 0x09);

    // End of a 30-day month rolls the month.
    sub.setDateTime({0x04, 0x30, 0x23, 0x59, 0x59});
    sub.tickOneSecond();
    dt = sub.dateTime();
    CHECK(dt.day == 0x01 && dt.month == 0x05);

    // Non-leap February stops at 28; leap February reaches 29.
    sub.setYear(2025);
    sub.setDateTime({0x02, 0x28, 0x23, 0x59, 0x59});
    sub.tickOneSecond();
    CHECK(sub.dateTime().month == 0x03 && sub.dateTime().day == 0x01);
    sub.setYear(2024);
    sub.setDateTime({0x02, 0x28, 0x23, 0x59, 0x59});
    sub.tickOneSecond();
    CHECK(sub.dateTime().month == 0x02 && sub.dateTime().day == 0x29);

    // 31 Dec 23:59:59 -> 1 Jan, year advances.
    sub.setYear(2025);
    sub.setDateTime({0x0C, 0x31, 0x23, 0x59, 0x59});
    sub.tickOneSecond();
    dt = sub.dateTime();
    CHECK(dt.month == 0x01 && dt.day == 0x01);
    CHECK(sub.year() == 2026);
}

void test_subcpu_host_seed_survives_cold_init() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    auto& bus = static_cast<SC7852Bus&>(mem);
    auto& sub = mem.subCpu();

    // Host seeds a real time and arms the guard (as PC1600Machine::seedClock
    // does).
    sub.setDateTime({0x09, 0x01, 0x21, 0x34, 0x56});
    sub.setYear(2026);
    sub.armHostSeedGuard();

    // The boot ROM's cold-start write: 6DH with 1 Jan 00:00:00 -> swallowed.
    const uint8_t cold[9] = {0x1, 0x0, 0x1, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
    bus.writeIO(0x21, static_cast<uint8_t>(0x00 | (0x0F - cold[0])));
    for (int i = 1; i < 9; i++)
        bus.writeIO(0x21, static_cast<uint8_t>(0x70 | (0x0F - cold[i])));
    bus.writeIO(0x21, 0x6D);
    PC1600SubCpu::DateTime dt = sub.dateTime();
    CHECK(dt.month == 0x09 && dt.day == 0x01 && dt.hour == 0x21);

    // A later real TIME$= write (guard already spent) applies normally.
    const uint8_t set[9] = {0x0C, 0x2, 0x5, 0x0, 0x8, 0x1, 0x5, 0x3, 0x0};
    bus.writeIO(0x21, static_cast<uint8_t>(0x00 | (0x0F - set[0])));
    for (int i = 1; i < 9; i++)
        bus.writeIO(0x21, static_cast<uint8_t>(0x70 | (0x0F - set[i])));
    bus.writeIO(0x21, 0x6D);
    dt = sub.dateTime();
    CHECK(dt.month == 0x0C && dt.day == 0x25 && dt.hour == 0x08 &&
          dt.minute == 0x15 && dt.second == 0x30);
}

void test_subcpu_interrupt_mask_and_password() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    auto& bus = static_cast<SC7852Bus&>(mem);

    // 5FH [A0H SWMSK] builds the interrupt mask from the first two nibbles.
    bus.writeIO(0x21, static_cast<uint8_t>(0x00 | (0x0F - 0x0A)));
    bus.writeIO(0x21, static_cast<uint8_t>(0x70 | (0x0F - 0x05)));
    bus.writeIO(0x21, 0x5F);
    CHECK(mem.subCpu().interruptMask() == 0xA5);
    bus.writeIO(0x21, 0x5E);            // read it back
    CHECK(bus.readIO(0x33) == 0xA5);

    // 65H [9AH, IOCS 0AH] stores a password; 5CH then reports bit2 set alongside bit5.
    CHECK(!mem.subCpu().passwordSet());
    bus.writeIO(0x21, static_cast<uint8_t>(0x00 | (0x0F - 0x07)));
    bus.writeIO(0x21, 0x65);
    CHECK(mem.subCpu().passwordSet());
    bus.writeIO(0x21, 0x5C);
    CHECK(bus.readIO(0x33) == 0x24);
}

void test_memory_intmask_reads_back_via_port35() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    auto& bus = static_cast<SC7852Bus&>(mem);
    bus.writeIO(0x35, 0xFF);
    CHECK(bus.readIO(0x35) == 0xFF);
    bus.writeIO(0x35, 0x01);   // what the timer ISR sets at 4117H
    CHECK(bus.readIO(0x35) == 0x01);
    CHECK(mem.intMask() == 0x01);
}

// The romaji->kana caption is on commons X35 / X59 (Service Manual glass
// pinout): page 4 bit 2 and page 7 bit 2 of IC3 column 63. KBII's bit 7
// has no electrode, and the KBII mode flag in RAM must not light anything.
void test_romaji_kana_segments_follow_the_glass_pinout() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    auto& bus = static_cast<SC7852Bus&>(mem);
    using Symbol = PC1600StatusLine::Symbol;
    bus.writeIO(0x54, 0x3F);        // display on
    bus.writeIO(0x54, 0x40 | 63);   // column 63

    bus.writeIO(0x54, 0xB8 | 4);    // page 4
    bus.writeIO(0x56, 0x04);        // bit 2 = X35
    CHECK(mem.display().statusLine().isOn(Symbol::Romaji));
    CHECK(!mem.display().statusLine().isOn(Symbol::Kana));
    CHECK(!mem.display().statusLine().isOn(Symbol::S));

    bus.writeIO(0x54, 0x40 | 63);
    bus.writeIO(0x56, 0x80);        // bit 7 (KBII): no electrode
    CHECK(!mem.display().statusLine().isOn(Symbol::Romaji));
    CHECK(!mem.display().statusLine().isOn(Symbol::Kana));
    CHECK(!mem.display().statusLine().isOn(Symbol::S));

    bus.writeIO(0x54, 0xB8 | 7);    // page 7
    bus.writeIO(0x54, 0x40 | 63);
    bus.writeIO(0x56, 0x04);        // bit 2 = X59
    CHECK(mem.display().statusLine().isOn(Symbol::Kana));
    CHECK(!mem.display().statusLine().isOn(Symbol::Busy));
    CHECK(!mem.display().statusLine().isOn(Symbol::Small));

    mem.write(0xF3C6, 0x84);        // RAM shadow only
    bus.writeIO(0x54, 0xB8 | 4);
    bus.writeIO(0x54, 0x40 | 63);
    bus.writeIO(0x56, 0x00);
    CHECK(!mem.display().statusLine().isOn(Symbol::Romaji));
}

void test_subcpu_interrupt_cause_bit6() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    auto& bus = static_cast<SC7852Bus&>(mem);
    auto& sub = mem.subCpu();
    auto cause6 = [&] { return static_cast<uint8_t>(bus.readIO(0x32) & 0x40); };

    // Bit 6 is the sub-CPU's Z7: pending events its own mask enables.
    // Without SWMSK the 0.5 s tick stays pending but raises nothing.
    sub.halfSecondTick();
    CHECK(cause6() == 0x00);
    // SWMSK 02H (5FH [A0H] after the nibbles 0, 2, sent complemented).
    bus.writeIO(0x21, 0x0F);
    bus.writeIO(0x21, 0x7D);
    bus.writeIO(0x21, 0x5F);
    CHECK(cause6() == 0x40);
    CHECK(cause6() == 0x40);              // a level: a 32H read doesn't clear it

    // SRIRQ (5DH [A2H]) returns the pending bits and clears them, which
    // drops the line (Service Manual §4-3).
    bus.writeIO(0x21, 0x5D);
    CHECK(bus.readIO(0x33) == 0x02);
    CHECK(cause6() == 0x00);
    bus.writeIO(0x21, 0x5D);
    CHECK(bus.readIO(0x33) == 0x00);

    // A masked-off event is reported by SRIRQ too, and cleared with it.
    sub.tickOneSecond();                  // 1 s bit, not in the mask
    CHECK(cause6() == 0x00);
    bus.writeIO(0x21, 0x5D);
    CHECK(bus.readIO(0x33) == 0x04);
}

// SWA1T / SRA1T (96H / 97H) store and read back a timer; at the minute
// carry that matches it, the sub-CPU raises its SRIRQ bit. '?' fields are
// sent as F nibbles and match anything (SubCpu §7.3).
void test_subcpu_timer_store_readback_and_match() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    auto& sub = mem.subCpu();
    auto send = [&](std::initializer_list<uint8_t> nibbles, uint8_t exec) {
        bool first = true;
        for (uint8_t n : nibbles) {
            sub.strobe(static_cast<uint8_t>((first ? 0xF0 : 0x80) | n));
            first = false;
        }
        sub.strobe(exec);
    };
    sub.setDateTime({0x09, 0x26, 0x13, 0x29, 0x58});
    // ON TIME$ = "??/??/13/30" (month F, day FF, 13:30, seconds sent too).
    send({0xF, 0xF, 0xF, 0x1, 0x3, 0x3, 0x0, 0x0, 0x0}, 0x96);
    sub.strobe(0x97);
    const uint8_t want[7] = {0xF, 0xF, 0xF, 0x1, 0x3, 0x3, 0x0};
    for (uint8_t w : want) { sub.strobe(0x90); CHECK(sub.readAnswer() == w); }

    sub.strobe(0xA2); (void)sub.readAnswer();
    sub.tickOneSecond();                  // 13:29:59
    CHECK((sub.pendingInterrupts() & PC1600SubCpu::kIrqAlarm1) == 0);
    sub.tickOneSecond();                  // 13:30:00 -- match
    CHECK((sub.pendingInterrupts() & PC1600SubCpu::kIrqAlarm1) != 0);

    // A cleared timer (month 0, SINIT's default) never fires.
    send({0x0, 0, 0, 0, 0, 0, 0, 0, 0}, 0x98);
    sub.strobe(0xA2); (void)sub.readAnswer();
    for (int i = 0; i < 120; i++) sub.tickOneSecond();
    CHECK((sub.pendingInterrupts() & PC1600SubCpu::kIrqAlarm2) == 0);

    // WAKE$(0) at an exact date and time.
    sub.setDateTime({12, 0x25, 0x07, 0x29, 0x59}); // month is plain binary
    send({0xC, 0x2, 0x5, 0x0, 0x7, 0x3, 0x0, 0x0, 0x0}, 0x94);
    sub.tickOneSecond();
    CHECK((sub.pendingInterrupts() & PC1600SubCpu::kIrqWakeUp) != 0);
}

void test_pb3_reads_high_for_the_alternate_charset_gate() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    auto& bus = static_cast<SC7852Bus&>(mem);

    // PB3 latches its externally-pulled-up pin (PCSTB, pin 78) at reset and
    // reads 1 forever after. The boot ROM copies exactly this bit into
    // F1BCH bit 7 (PC1600-P0-B0-new.bin 0512H), which gates the KBII key's whole
    // toggle (PC1600-P1-B0-new.bin 6CC1H). Reading 0 here makes KBII inert.
    CHECK((bus.readIO(0x1F) & 0x08) != 0);

    // It is an input pin, so the ROM's own read-modify-write of OPB must
    // not be able to clear it -- replay the key-scan strobe pair.
    bus.writeIO(0x1D, static_cast<uint8_t>(bus.readIO(0x1D) | 0x40));
    bus.writeIO(0x1F, static_cast<uint8_t>(bus.readIO(0x1F) & 0xBF));
    CHECK((bus.readIO(0x1F) & 0x08) != 0);
    bus.writeIO(0x1F, 0x00);      // a plain write, not read-modify-write
    CHECK((bus.readIO(0x1F) & 0x08) != 0);

    // Survives reset (the flip-flop re-latches the same pull-up), and so
    // does a running PB5, which is not a reset-latched line.
    mem.setTimer64Bit(true);
    mem.reset();
    CHECK((bus.readIO(0x1F) & 0x08) != 0);
    CHECK((bus.readIO(0x1F) & 0x20) != 0);
}

void test_memory_display_wiring_via_io() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    static_cast<SC7852Bus&>(mem).writeIO(0x58, 0x3F);
    static_cast<SC7852Bus&>(mem).writeIO(0x58, 0xB8); // page 0
    static_cast<SC7852Bus&>(mem).writeIO(0x58, 0x40);
    static_cast<SC7852Bus&>(mem).writeIO(0x5A, 0x01);
    CHECK(mem.display().pixel(0, 0));
}

void test_memory_clock_enable_via_port37_write() {
    PC1600Bank bank;
    PC1600Memory mem(bank);
    CHECK(!mem.display().clockEnabled());
    static_cast<SC7852Bus&>(mem).writeIO(0x37, 0x10); // bit4 set
    CHECK(mem.display().clockEnabled());
    // Reset clears port 37H, so CK0 stops; the LCD RAM stays (VGG).
    static_cast<SC7852Bus&>(mem).writeIO(0x58, 0x3F);
    static_cast<SC7852Bus&>(mem).writeIO(0x58, 0xB8);
    static_cast<SC7852Bus&>(mem).writeIO(0x58, 0x40);
    static_cast<SC7852Bus&>(mem).writeIO(0x5A, 0x01);
    mem.reset();
    CHECK(!mem.display().clockEnabled());
    CHECK(mem.display().pixel(0, 0));
}

} // namespace

// The GUI/typer's PC-1600 digit-row SHIFT table must agree with the ROM's
// own SHIFT-code table (SFTCDT, bank 6 @ 953FH, indexed by key code - 08H;
// PC-1600-Keyboard.md §7). Regression: '_' used to map to SHIFT + 9, but
// SFTCDT puts it on "." and leaves 9 unshifted.
void test_digit_row_shift_table_matches_rom_sftcdt() {
    std::ifstream in("roms/PC1600-P2-B6-new.bin", std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "SKIP test_digit_row_shift_table_matches_rom_sftcdt: roms/PC1600-P2-B6-new.bin not found\n");
        return;
    }
    const std::vector<unsigned char> rom((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const size_t kSftcdt = 0x953F - 0x8000; // bank 6 is mapped at 8000H
    CHECK(rom.size() == 0x4000);
    if (rom.size() != 0x4000) return;

    int checked = 0;
    for (int c = 0x20; c < 0x7F; ++c) {
        std::string base;
        if (!pc1600DigitRowShiftedBaseKey(static_cast<char>(c), &base)) continue;
        const int code = base == "space" ? 0x20 : static_cast<unsigned char>(base[0]);
        const bool matches = rom[kSftcdt + static_cast<size_t>(code - 0x08)] == c;
        if (!matches) std::fprintf(stderr, "  SFTCDT mismatch: '%c' mapped to SHIFT + %s\n", c, base.c_str());
        CHECK(matches);
        ++checked;
    }
    CHECK(checked == 11);

    std::string base;
    CHECK(pc1600DigitRowShiftedBaseKey('_', &base) && base == ".");
    CHECK(rom[kSftcdt + ('9' - 0x08)] == '9'); // SHIFT + 9 has no second legend
}

int run_pc1600_keyboard_display_tests() {
    test_keyboard_name_lookup();
    test_keyboard_scan_single_strobe();
    test_keyboard_pb6_strobe();
    test_keyboard_rsv_matrix_position();
    test_keyboard_no_keys_pressed_is_all_ones();
    test_display_ic2_command_and_data();
    test_display_read_returns_output_register();
    test_display_cursor_style_read_modify_write_roundtrip();
    test_display_ic3_column_offset();
    test_display_off_reads_as_blank();
    test_display_rightmost_columns_read_ic2_pages4to7();
    test_display_start_line_rotates_visible_window();
    test_display_status_symbols_wired_to_ic3_column63();
    test_display_status_symbols_blank_when_ic3_display_off();
    test_display_clock_enable_flag();
    test_display_busy_after_write();
    test_display_busy_holds_while_clock_off();
    test_display_status_reports_on_off();
    test_statusline_defaults_all_off();
    test_statusline_set_and_read();
    test_statusline_reset_clears_all();
    test_display_owns_status_line();
    test_memory_keyboard_and_on_key_via_io();
    test_memory_pb6_strobe_needs_output_mode_via_io();
    test_memory_pb5_survives_opb_read_modify_write();
    test_subcpu_requests_answer_on_port33();
    test_subcpu_nibble_stack_clock_roundtrip();
    test_subcpu_clock_ticks_and_rolls_over();
    test_subcpu_host_seed_survives_cold_init();
    test_subcpu_interrupt_mask_and_password();
    test_memory_intmask_reads_back_via_port35();
    test_romaji_kana_segments_follow_the_glass_pinout();
    test_subcpu_interrupt_cause_bit6();
    test_subcpu_timer_store_readback_and_match();
    test_pb3_reads_high_for_the_alternate_charset_gate();
    test_memory_display_wiring_via_io();
    test_memory_clock_enable_via_port37_write();
    test_digit_row_shift_table_matches_rom_sftcdt();

    std::printf("pc1600_keyboard_display_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
