// Headless tests for "Load Machine Code…": Core/MachineCodeFile (header
// recognition, the load plan, the NEW/CALL advice, hex-address parsing)
// and the two writers, PC1500MachineCodeLoader / PC1600MachineCodeLoader.
// Same no-framework, assert-and-tally style as the other Core test files.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../MachineCodeFile.hpp"
#include "../PC1500/PC1500MachineCodeLoader.hpp"
#include "../PC1500/PC1500Machine.hpp"
#include "../PC1600/PC1600MachineCodeLoader.hpp"
#include "../PC1600/PC1600BasicTyper.hpp"
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

using machinecode::File;
using machinecode::Slot;
using machinecode::Target;

const std::vector<uint8_t> kCode = {0x3E, 0x41, 0xC9, 0x00, 0x11};  // 5 bytes

std::vector<uint8_t> pc1600File(const std::vector<uint8_t>& payload, uint32_t load, uint32_t autorun,
                                uint8_t type = 0x10) {
    std::vector<uint8_t> f = {0xFF, 0x10, 0x00, 0x00, type};
    const uint32_t n = static_cast<uint32_t>(payload.size());
    for (uint32_t v : {n, load, autorun}) {
        f.push_back(static_cast<uint8_t>(v & 0xFF));
        f.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        f.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    }
    f.push_back(0x00);
    f.push_back(0x0F);
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

std::vector<uint8_t> ce158File(const std::vector<uint8_t>& payload, uint16_t load, uint16_t autorun,
                               uint8_t type = 0x42) {
    std::vector<uint8_t> f = {0x01, type, 'C', 'O', 'M'};
    const char name[16] = "TEST";
    f.insert(f.end(), name, name + 16);
    const uint16_t lenField = static_cast<uint16_t>(payload.size() - 1);  // stored as length - 1
    for (uint16_t v : {load, lenField, autorun}) {
        f.push_back(static_cast<uint8_t>(v >> 8));
        f.push_back(static_cast<uint8_t>(v & 0xFF));
    }
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

// ── readFile ─────────────────────────────────────────────────────────────

void test_read_ce158() {
    File f = machinecode::readFile(ce158File(kCode, 0x40C5, 0x40C7));
    CHECK(f.ok);
    CHECK(f.header == File::Header::CE158);
    CHECK(f.loadAddr == 0x40C5);
    CHECK(f.autorunAddr == 0x40C7);
    CHECK(f.payload == kCode);
}

void test_read_ffff_autorun_is_none() {
    // &FFFF is the header's "no auto-start" default -- no XCALL/CALL advice.
    CHECK(machinecode::readFile(ce158File(kCode, 0x40C5, 0xFFFF)).autorunAddr == 0);
    CHECK(machinecode::readFile(pc1600File(kCode, 0xC0C5, 0xFFFF)).autorunAddr == 0);
}

void test_read_ce158_length_mismatch() {
    auto bytes = ce158File(kCode, 0x40C5, 0);
    bytes.push_back(0xAA);  // one byte more than the header's (length - 1) + 1
    File f = machinecode::readFile(bytes);
    CHECK(!f.ok);
    CHECK(f.header == File::Header::CE158);
    CHECK(!f.error.empty());
    // The header's fields are still there for a preset's `length:` override.
    CHECK(f.lengthMismatch);
    CHECK(f.loadAddr == 0x40C5);
    CHECK(f.payload.size() == kCode.size() + 1);
}

void test_read_ce158_basic_type_rejected() {
    File f = machinecode::readFile(ce158File(kCode, 0, 0, /*type=*/0x40));
    CHECK(!f.ok);
    CHECK(f.error.find("not machine code") != std::string::npos);
}

void test_read_pc1600() {
    File f = machinecode::readFile(pc1600File(kCode, 0xC0C5, 0));
    CHECK(f.ok);
    CHECK(f.header == File::Header::PC1600);
    CHECK(f.loadAddr == 0xC0C5);
    CHECK(f.autorunAddr == 0);
    CHECK(f.payload == kCode);
}

void test_read_pc1600_basic_type_rejected() {
    File f = machinecode::readFile(pc1600File(kCode, 0, 0, /*type=*/0x21));
    CHECK(!f.ok);
    CHECK(f.error.find("Load BASIC Program") != std::string::npos);
}

void test_read_basic_listing_rejected() {
    const std::string bas = "10 PRINT \"HI\"\n20 END\n";
    File f = machinecode::readFile(std::vector<uint8_t>(bas.begin(), bas.end()));
    CHECK(!f.ok);
    CHECK(f.error.find("BASIC listing") != std::string::npos);
    CHECK(f.error.find("Load BASIC Program") != std::string::npos);
}

void test_read_other_kinds_rejected() {
    const std::string notes = "Just some notes,\nnot a program.\n";
    File text = machinecode::readFile(std::vector<uint8_t>(notes.begin(), notes.end()));
    CHECK(!text.ok);
    CHECK(text.error.find("plain text") != std::string::npos);
    // A CE-158 header of an unknown type ('Z'): magic without a known header.
    File cut = machinecode::readFile(ce158File(kCode, 0x40C5, 0, /*type=*/'Z'));
    CHECK(!cut.ok);
    CHECK(cut.error.find("no complete header") != std::string::npos);
}

void test_read_truncated() {
    auto bytes = pc1600File(kCode, 0xC0C5, 0);
    bytes.pop_back();
    File f = machinecode::readFile(bytes);
    CHECK(!f.ok);
    CHECK(f.lengthMismatch);
    CHECK(f.error.find("promises more code") != std::string::npos);
    CHECK(f.payload.size() == kCode.size() - 1);
}

// `00` bytes before the header (e.g. from a serial capture) are skipped.
void test_read_leading_noise() {
    auto bytes = ce158File(kCode, 0x40C5, 0x40C7);
    bytes.insert(bytes.begin(), {0x00, 0x00});
    File f = machinecode::readFile(bytes);
    CHECK(f.ok);
    CHECK(f.header == File::Header::CE158);
    CHECK(f.payload == kCode);
}

void test_read_headerless() {
    File f = machinecode::readFile(kCode);
    CHECK(f.ok);
    CHECK(f.header == File::Header::None);
    CHECK(f.payload == kCode);
}

// ── plan ─────────────────────────────────────────────────────────────────

using machinecode::BasicArea;
using machinecode::Cpu;
using machinecode::PC1600State;

const std::vector<BasicArea> kStockAreas = {{0, 0xC000, 0xEFFF, 0}};
const std::vector<BasicArea> kSlot1FirstAreas = {{1, 0x8000, 0xBFFF, 0}, {0, 0xC000, 0xEFFF, 0}};
const std::vector<BasicArea> kSlot2FirstAreas = {{2, 0x8000, 0xBFFF, 0}, {0, 0xC000, 0xEFFF, 0}};

PC1600State state(const std::vector<BasicArea>& areas, bool mode1 = false, int title = 0) {
    PC1600State st;
    st.mode1 = mode1;
    st.title = title;
    st.basicAreas = areas;
    st.ramEnd = 0xEB00;
    if (title != 0) {
        st.titleBase = 0x8000;
        st.titleStart = 0x80C5;
    }
    return st;
}
const PC1600State kStock = state(kStockAreas);
const PC1600State kSlot1First = state(kSlot1FirstAreas);
const PC1600State kSlot2First = state(kSlot2FirstAreas);

void test_plan_model_mismatch() {
    // A CE-158 (PC-1500) file: the PC-1600 takes it in MODE 1 only.
    auto ce158 = machinecode::readFile(ce158File(kCode, 0x40C5, 0));
    auto p = machinecode::plan(Target::PC1600, ce158, kStock);
    CHECK(p.error.find("MODE 1") != std::string::npos);
    p = machinecode::plan(Target::PC1600, ce158, state(kStockAreas, true));
    CHECK(p.error.empty() && p.cpu == Cpu::LH5803 && p.busAddr == 0xC0C5 && p.slot == Slot::S0);
    CHECK(machinecode::plan(Target::PC1500, ce158, {}).error.empty());

    // A PC-1600 file is Z-80 code in either MODE.
    auto pc1600 = machinecode::readFile(pc1600File(kCode, 0xC0C5, 0));
    CHECK(!machinecode::plan(Target::PC1500, pc1600, {}).error.empty());
    CHECK(machinecode::plan(Target::PC1600, pc1600, kStock).error.empty());
    p = machinecode::plan(Target::PC1600, pc1600, state(kStockAreas, true));
    CHECK(p.error.empty() && p.cpu == Cpu::Z80 && p.busAddr == 0xC0C5);
}

void test_plan_headerless_needs_address() {
    auto f = machinecode::readFile(kCode);
    auto p1500 = machinecode::plan(Target::PC1500, f, {});
    CHECK(p1500.error.empty() && p1500.needsAddress && p1500.defaultAddr == 0);

    // PC-1600 default: the start of the selected program area, in the
    // MODE's address space.
    auto p1600 = machinecode::plan(Target::PC1600, f, kStock);
    CHECK(p1600.error.empty() && p1600.needsAddress && p1600.defaultAddr == 0xC0C5 && p1600.cpu == Cpu::Z80);
    CHECK(machinecode::plan(Target::PC1600, f, kSlot2First).defaultAddr == 0x80C5);
    CHECK(machinecode::plan(Target::PC1600, f, PC1600State{}).defaultAddr == 0xC0C5);
    auto m1 = machinecode::plan(Target::PC1600, f, state(kStockAreas, true));
    CHECK(m1.cpu == Cpu::LH5803 && m1.defaultAddr == 0x40C5);
    // TITLE "S1:": the program module's start.
    CHECK(machinecode::plan(Target::PC1600, f, state(kStockAreas, false, 1)).defaultAddr == 0x80C5);
}

void test_plan_pc1600_target() {
    // $C000-$FFFF always goes to internal RAM.
    auto s0 = machinecode::readFile(pc1600File(kCode, 0xC0C5, 0));
    auto p = machinecode::plan(Target::PC1600, s0, kSlot1First);
    CHECK(p.error.empty() && p.slot == Slot::S0);

    // $8000-$BFFF: the module behind the selected program area.
    auto slot = machinecode::readFile(pc1600File(kCode, 0x80C5, 0));
    p = machinecode::plan(Target::PC1600, slot, kStock);
    CHECK(p.error.find("TITLE") != std::string::npos);
    p = machinecode::plan(Target::PC1600, slot, kSlot1First);
    CHECK(p.error.empty() && p.slot == Slot::S1);
    p = machinecode::plan(Target::PC1600, slot, kSlot2First);
    CHECK(p.error.empty() && p.slot == Slot::S2);
    CHECK(!machinecode::plan(Target::PC1600, slot, PC1600State{}).error.empty());
    // TITLE "S2:": the slot 2 program module, whatever S0 holds.
    p = machinecode::plan(Target::PC1600, slot, state(kSlot1FirstAreas, false, 2));
    CHECK(p.error.empty() && p.slot == Slot::S2);

    // An 8 KB module's window starts at $A000.
    CHECK(!machinecode::plan(Target::PC1600, slot, state({{1, 0xA000, 0xBFFF, 0}, {0, 0xC000, 0xEFFF, 0}})).error.empty());

    auto rom = machinecode::readFile(pc1600File(kCode, 0x7000, 0));
    CHECK(!machinecode::plan(Target::PC1600, rom, kSlot1First).error.empty());

    // $BFFE + 5 bytes crosses into $C000.
    auto crossing = machinecode::readFile(pc1600File(kCode, 0xBFFE, 0));
    CHECK(!machinecode::plan(Target::PC1600, crossing, kSlot1First).error.empty());

    // The work area is allowed, with a warning -- many programs live up
    // there, e.g. CLOCK.BIN at &FF3A-&FFFB (WAKE$ + the CE-1F01A pen area).
    auto work = machinecode::readFile(pc1600File(kCode, 0xF800, 0));
    CHECK(machinecode::plan(Target::PC1600, work, kStock).error.empty());
    CHECK(machinecode::pc1600WorkAreaWarning(0xF800, kCode.size(), Cpu::Z80).find("work area") != std::string::npos);
    auto clock = machinecode::readFile(pc1600File(kCode, 0xFF3A, 0xFF3A));
    CHECK(machinecode::plan(Target::PC1600, clock, kStock).error.empty());
    CHECK(machinecode::pc1600WorkAreaWarning(0xFF3A, 0xC2, Cpu::Z80).find("WAKE$") != std::string::npos);
    CHECK(machinecode::pc1600WorkAreaWarning(0xFF40, 0xC0, Cpu::Z80).find("CE-1F01A") != std::string::npos);
    auto sys = machinecode::readFile(ce158File(kCode, 0x7800, 0));
    auto pp = machinecode::plan(Target::PC1600, sys, state(kStockAreas, true));
    CHECK(pp.error.empty() && pp.busAddr == 0xF800 && pp.slot == Slot::S0);
    const std::string w = machinecode::pc1600WorkAreaWarning(0xF800, kCode.size(), Cpu::LH5803);
    CHECK(w.find("LH5803 &7000") != std::string::npos && w.find("7C01") != std::string::npos);
    CHECK(machinecode::pc1600WorkAreaWarning(0xEF00, 0x10, Cpu::Z80).empty());

    // In MODE 1 a refusal names the address as typed, in the LH5803 view.
    auto lhModule = machinecode::readFile(ce158File(kCode, 0x00C5, 0));
    p = machinecode::plan(Target::PC1600, lhModule, state(kStockAreas, true));
    CHECK(p.error.find("LH5803 &C5") != std::string::npos && p.error.find("&40C5") != std::string::npos);
    CHECK(p.error.find("&80C5") == std::string::npos);
}

void test_plan_load_configurations() {
    using machinecode::LoadError;
    using machinecode::LoadOptions;
    // A header whose length disagrees with the file (one byte too many).
    std::vector<uint8_t> longer = pc1600File(kCode, 0xC0C5, 0);
    longer.push_back(0x00);
    const File mismatched = machinecode::readFile(longer);
    CHECK(!mismatched.ok && mismatched.lengthMismatch);
    const File headerless = machinecode::readFile(kCode);

    // Build & Load: a mismatch loads; the debugger names the CPU; LH5803
    // addresses map to the Z-80's 8000-FFFF.
    LoadOptions dbg;
    dbg.target = Target::PC1600;
    dbg.acceptLengthMismatch = true;
    dbg.hasCpu = true;
    auto p = machinecode::planLoad(mismatched, dbg, kStock);
    CHECK(p.error == LoadError::None && p.slot == Slot::S0 && p.busAddr == 0xC0C5);
    CHECK(machinecode::planLoad(headerless, dbg, kStock).error == LoadError::NeedsAddress);
    LoadOptions lh = dbg;
    lh.cpu = Cpu::LH5803;
    lh.hasAddress = true;
    lh.address = 0x1000;
    p = machinecode::planLoad(headerless, lh, kSlot1First);
    CHECK(p.error == LoadError::None && p.addr == 0x1000 && p.busAddr == 0x9000 && p.slot == Slot::S1);
    CHECK(machinecode::planLoad(headerless, lh, kStock).error == LoadError::NoSlot); // 9000 with no module first
    lh.address = 0x7FFE;
    CHECK(machinecode::planLoad(headerless, lh, kSlot1First).error == LoadError::LhRange);
    LoadOptions atEnd = dbg;
    atEnd.hasAddress = true;
    atEnd.address = 0xFFFE;
    p = machinecode::planLoad(headerless, atEnd, kStock);
    CHECK(p.error == LoadError::NoSlot && p.detail.find("runs past") != std::string::npos);
    atEnd.address = 0x10000;
    CHECK(machinecode::planLoad(headerless, atEnd, kStock).error == LoadError::OutsideBank0);

    // Without a CPU from the caller, the MODE decides a headerless file's.
    LoadOptions gui;
    gui.target = Target::PC1600;
    gui.hasAddress = true;
    gui.address = 0x40C5;
    p = machinecode::planLoad(headerless, gui, state(kStockAreas, true));
    CHECK(p.error == LoadError::None && p.cpu == Cpu::LH5803 && p.busAddr == 0xC0C5);
    CHECK(machinecode::planLoad(headerless, gui, kStock).error == LoadError::NoSlot);  // Z-80 &40C5 is ROM

    // Presets: `length:` trims and accepts a mismatch; the target follows
    // MODE / TITLE; the range is the writer's business.
    LoadOptions preset;
    preset.target = Target::PC1600;
    preset.checkRange = false;
    CHECK(machinecode::planLoad(mismatched, preset, kStock).error == LoadError::BadFile);
    preset.hasLength = preset.acceptLengthMismatch = true; // as PresetRunner sets them
    preset.length = 3;
    p = machinecode::planLoad(mismatched, preset, kStock);
    CHECK(p.error == LoadError::None && p.len == 3 && p.slot == Slot::S0 && p.addr == 0xC0C5);
    preset.length = 99;
    CHECK(machinecode::planLoad(mismatched, preset, kStock).error == LoadError::LengthExceeds);
    preset.length = 0;
    CHECK(machinecode::planLoad(mismatched, preset, kStock).error == LoadError::Empty);
    preset.hasLength = preset.acceptLengthMismatch = false;
    CHECK(machinecode::planLoad(headerless, preset, kStock).error == LoadError::NeedsAddress);

    // Load Machine Code… (plan()): a CE-158 header running past &FFFF.
    const File past = machinecode::readFile(ce158File(kCode, 0xFFFE, 0));
    CHECK(machinecode::plan(Target::PC1500, past, {}).error.find("runs past") != std::string::npos);
    CHECK(machinecode::plan(Target::PC1500, mismatched, {}).error == mismatched.error);
}

// ── advice ───────────────────────────────────────────────────────────────

void test_advice_pc1500() {
    // PC-1500A-style RAM $4000-$57FF: BASIC can start at $40C5 at the lowest.
    auto a = machinecode::advice(Target::PC1500, Slot::S0, 0, 0x40C5, 0x20, 0, 0x4000, 0x5800, {}, Cpu::Z80);
    CHECK(a.newCommand == "NEW &40E5");
    CHECK(a.callCommand == "CALL &40C5");
    CHECK(a.callNote.find("first byte") != std::string::npos);

    a = machinecode::advice(Target::PC1500, Slot::S0, 0, 0x4010, 0x20, 0x4012, 0x4000, 0x5800, {}, Cpu::Z80);
    CHECK(a.newCommand.empty());
    CHECK(a.newNote.find("reserve") != std::string::npos);
    CHECK(a.callCommand == "CALL &4012");
    CHECK(a.callNote.find("auto-run") != std::string::npos);

    a = machinecode::advice(Target::PC1500, Slot::S0, 0, 0x7C01, 0x10, 0, 0x4000, 0x5800, {}, Cpu::Z80);
    CHECK(a.newCommand.empty());
    CHECK(a.newNote.find("outside") != std::string::npos);
}

void test_advice_pc1600() {
    // Stock: the S0 area starts in internal RAM.
    auto a = machinecode::advice(Target::PC1600, Slot::S0, 0, 0xC0C5, 0x100, 0, 0, 0, kStock, Cpu::Z80);
    CHECK(a.newCommand == "NEW \"S0:\",&1C5");
    CHECK(a.newNote.find("Warning") == std::string::npos);
    CHECK(a.callCommand == "CALL &C0C5");

    a = machinecode::advice(Target::PC1600, Slot::S0, 0, 0xC000, 0x10, 0, 0, 0, kStock, Cpu::Z80);
    CHECK(a.newNote.find("Warning") != std::string::npos);

    // Top of the work area: WAKE$ storage, then the de-facto free FF40-FFFF.
    a = machinecode::advice(Target::PC1600, Slot::S0, 0, 0xFF3A, 0xC2, 0, 0, 0, kStock, Cpu::Z80);
    CHECK(a.newCommand.empty());
    CHECK(a.newNote.find("WAKE$") != std::string::npos);
    a = machinecode::advice(Target::PC1600, Slot::S0, 0, 0xFF40, 0xBC, 0, 0, 0, kStock, Cpu::Z80);
    CHECK(a.newCommand.empty());
    CHECK(a.newNote.find("Warning") == std::string::npos);
    CHECK(a.newNote.find("CE-1F01A") != std::string::npos);

    // Module in slot 1: BASIC starts there, so NEW "S0:" counts from $8000.
    a = machinecode::advice(Target::PC1600, Slot::S1, 0, 0x80C5, 0x40, 0x80D0, 0, 0, kSlot1First, Cpu::Z80);
    CHECK(a.newCommand == "NEW \"S0:\",&105");
    CHECK(a.callCommand == "CALL &80D0");

    // ... and internal RAM is the area's LAST run: NEW can't protect code there.
    a = machinecode::advice(Target::PC1600, Slot::S0, 0, 0xC0C5, 0x40, 0, 0, 0, kSlot1First, Cpu::Z80);
    CHECK(a.newCommand.empty());
    CHECK(a.newNote.find("&80C5") != std::string::npos);

    // Slot 2 outside the BASIC area: no NEW needed; CALL goes through bank 2.
    a = machinecode::advice(Target::PC1600, Slot::S2, 2, 0x80C5, 0x40, 0, 0, 0, kSlot1First, Cpu::Z80);
    CHECK(a.newCommand.empty());
    CHECK(a.newNote.find("doesn't use") != std::string::npos);
    CHECK(a.callCommand == "CALL #2,&80C5");

    a = machinecode::advice(Target::PC1600, Slot::S2, 2, 0x80C5, 0x40, 0, 0, 0, kSlot2First, Cpu::Z80);
    CHECK(a.newCommand == "NEW \"S0:\",&105");
    CHECK(a.callCommand == "CALL #2,&80C5");

    a = machinecode::advice(Target::PC1600, Slot::S0, 0, 0xC0C5, 0x40, 0, 0, 0, PC1600State{}, Cpu::Z80);
    CHECK(a.newCommand.empty());
    CHECK(a.newNote.find("couldn't be read") != std::string::npos);

    // TITLE "S1:": code in that program module is reserved with NEW "S1:".
    a = machinecode::advice(Target::PC1600, Slot::S1, 0, 0x80C5, 0x40, 0, 0, 0, state(kStockAreas, false, 1), Cpu::Z80);
    CHECK(a.newCommand == "NEW \"S1:\",&105");

    // MODE 1, LH5801 code: XCALL an LH5803 address; NEW <address> the
    // PC-1500 way, BASIC right after the code.
    const PC1600State mode1 = state(kStockAreas, true);
    a = machinecode::advice(Target::PC1600, Slot::S0, 0, 0x40C5, 0x40, 0, 0, 0, mode1, Cpu::LH5803);
    CHECK(a.callCommand == "XCALL &40C5");
    CHECK(a.newCommand == "NEW &4105");
    // MODE 1, Z-80 code (PC-1600 header): still CALL, NEW is the MODE's.
    a = machinecode::advice(Target::PC1600, Slot::S0, 0, 0xC0C5, 0x40, 0, 0, 0, mode1, Cpu::Z80);
    CHECK(a.callCommand == "CALL &C0C5");
    CHECK(a.newCommand == "NEW &4105");
}

void test_plan_header_bank() {
    // Bank 0 is "no bank given": an &80C5 file goes to S0's first module
    // run -- slot 2 (bank 2) when slot 2 starts S0, as with RAM in both slots.
    auto noBank = machinecode::readFile(pc1600File(kCode, 0x0080C5, 0));
    auto p = machinecode::plan(Target::PC1600, noBank, kSlot2First);
    CHECK(p.error.empty() && p.slot == Slot::S2 && p.bank == 2);
    p = machinecode::plan(Target::PC1600, noBank, kSlot1First);
    CHECK(p.error.empty() && p.slot == Slot::S1 && p.bank == 0);

    // A header bank 1-3 is honoured, whatever the program area is -- if
    // that bank has RAM under the code.
    PC1600State ram = kSlot1First;
    ram.bankRamPages = {~uint64_t{0}, ~uint64_t{0}, ~uint64_t{0}, ~uint64_t{0}};
    auto bank3 = machinecode::readFile(pc1600File(kCode, 0x0380C5, 0));
    p = machinecode::plan(Target::PC1600, bank3, ram);
    CHECK(p.error.empty() && p.slot == Slot::S2 && p.bank == 3 && p.busAddr == 0x80C5);
    auto bank1 = machinecode::readFile(pc1600File(kCode, 0x01A000, 0));
    p = machinecode::plan(Target::PC1600, bank1, kStock);
    CHECK(p.error.find("no RAM") != std::string::npos);  // no module at all
    PC1600State stockRam = state(kStockAreas);
    stockRam.bankRamPages[1] = ~uint64_t{0};
    p = machinecode::plan(Target::PC1600, bank1, stockRam);
    CHECK(p.error.empty() && p.slot == Slot::S1 && p.bank == 1 && p.busAddr == 0xA000);

    // RAM under only part of the code: refused, naming the bank.
    PC1600State partial = kSlot1First;
    partial.bankRamPages[3] = 1;  // page $80 only
    auto spill = machinecode::readFile(pc1600File(std::vector<uint8_t>(0x100, 0), 0x0380C5, 0));
    p = machinecode::plan(Target::PC1600, spill, partial);
    CHECK(p.error.find("bank 3") != std::string::npos && p.error.find("no RAM") != std::string::npos);

    // Only the module window has banks; only banks 0-3 are memory slots.
    auto internal = machinecode::readFile(pc1600File(kCode, 0x02C0C5, 0));
    CHECK(machinecode::plan(Target::PC1600, internal, ram).error.find("&8000-&BFFF") != std::string::npos);
    auto bank5 = machinecode::readFile(pc1600File(kCode, 0x0580C5, 0));
    CHECK(machinecode::plan(Target::PC1600, bank5, ram).error.find("banks 0-3") != std::string::npos);

    // An explicit address (preset `address:`, debugger) drops the header's bank.
    machinecode::LoadOptions o;
    o.target = Target::PC1600;
    o.hasAddress = true;
    o.address = 0xC0C5;
    auto lp = machinecode::planLoad(bank3, o, ram);
    CHECK(lp.error == machinecode::LoadError::None && lp.slot == Slot::S0 && lp.bank == 0);

    // The CALL goes through the bank; an auto-run bank of its own wins.
    auto a = machinecode::advice(Target::PC1600, Slot::S2, 3, 0x80C5, 0x40, 0, 0, 0, ram, Cpu::Z80);
    CHECK(a.callCommand == "CALL #3,&80C5");
    a = machinecode::advice(Target::PC1600, Slot::S1, 1, 0x80C5, 0x40, 0x0180D0, 0, 0, ram, Cpu::Z80);
    CHECK(a.callCommand == "CALL #1,&80D0");
    a = machinecode::advice(Target::PC1600, Slot::S2, 2, 0x80C5, 0x40, 0, 0, 0, kSlot2First, Cpu::Z80);
    CHECK(a.callCommand == "CALL #2,&80C5");
}

// ── parseHexAddress ──────────────────────────────────────────────────────

void test_parse_hex_address() {
    uint32_t v = 0;
    CHECK(machinecode::parseHexAddress("C0C5", &v) && v == 0xC0C5);
    CHECK(machinecode::parseHexAddress(" &c0c5 ", &v) && v == 0xC0C5);
    CHECK(machinecode::parseHexAddress("$40C5", &v) && v == 0x40C5);
    CHECK(machinecode::parseHexAddress("0x7C01", &v) && v == 0x7C01);
    CHECK(!machinecode::parseHexAddress("", &v));
    CHECK(!machinecode::parseHexAddress("&", &v));
    CHECK(!machinecode::parseHexAddress("12G4", &v));
    CHECK(!machinecode::parseHexAddress("10000", &v));
}

// ── writers ──────────────────────────────────────────────────────────────

void test_pc1600_writer() {
    PC1600Machine m;
    std::string err;
    CHECK(loadPC1600MachineCode(m, 0, 0xC0C5, kCode.data(), kCode.size(), &err));
    std::vector<uint8_t> ram(PC1600Machine::kInternalRamSize);
    m.debugCopyInternalRam(ram.data());
    CHECK(std::vector<uint8_t>(ram.begin() + 0xC5, ram.begin() + 0xC5 + 5) == kCode);

    // Slot 1: nothing attached -> refused; with 32 KB RAM -> lands at image offset $00C5.
    CHECK(!loadPC1600MachineCode(m, 1, 0x80C5, kCode.data(), kCode.size(), &err));
    m.memory().attachSlot1Card(plainRamCard(0x8000));
    CHECK(loadPC1600MachineCode(m, 1, 0x80C5, kCode.data(), kCode.size(), &err));
    std::vector<uint8_t> image = m.debugSlotImage(1);
    CHECK(image.size() >= 0xC5 + 5);
    if (image.size() >= 0xC5 + 5) CHECK(std::vector<uint8_t>(image.begin() + 0xC5, image.begin() + 0xC5 + 5) == kCode);

    // Bank 1: the module's upper 16 KB, image offset $40C5; bank 2 isn't slot 1's.
    CHECK(loadPC1600MachineCode(m, 1, 0x80C5, kCode.data(), kCode.size(), &err, 1));
    image = m.debugSlotImage(1);
    if (image.size() >= 0x40C5 + 5) CHECK(std::vector<uint8_t>(image.begin() + 0x40C5, image.begin() + 0x40C5 + 5) == kCode);
    CHECK(!loadPC1600MachineCode(m, 1, 0x80C5, kCode.data(), kCode.size(), &err, 2));

    // Which pages of banks 0-3 are RAM: a 32 KB module in slot 1 fills banks 0 and 1.
    const PC1600State st = pc1600LoadState(m);
    CHECK(st.bankRamPages[0] == ~uint64_t{0} && st.bankRamPages[1] == ~uint64_t{0});
    CHECK(st.bankRamPages[2] == 0 && st.bankRamPages[3] == 0);

    // Window checks.
    CHECK(!loadPC1600MachineCode(m, 0, 0xBFFF, kCode.data(), kCode.size(), &err));
    CHECK(!loadPC1600MachineCode(m, 1, 0xBFFE, kCode.data(), kCode.size(), &err));
}

void test_pc1500_writer() {
    PC1500Machine m;
    CHECK(m.loadROMFile("roms/PC-1500_A04.ROM"));
    std::string err;
    CHECK(loadPC1500MachineCode(m, 0x40C5, kCode.data(), kCode.size(), &err));
    for (size_t i = 0; i < kCode.size(); i++) CHECK(m.memory().peek(static_cast<uint16_t>(0x40C5 + i)) == kCode[i]);

    // System ROM at $C000: writes are dropped -> reported.
    CHECK(!loadPC1500MachineCode(m, 0xC000, kCode.data(), kCode.size(), &err));
    CHECK(err.find("not RAM") != std::string::npos);

    CHECK(!loadPC1500MachineCode(m, 0xFFFE, kCode.data(), kCode.size(), &err));
}

void test_pc1600_basic_areas() {
    {
        PC1600Machine m;
        CHECK(bootPC1600(m));
        auto areas = pc1600BasicAreas(m);
        CHECK(areas.size() == 1);
        if (!areas.empty()) CHECK(areas[0].slot == 0 && areas[0].windowBase == 0xC000);
    }
    {
        PC1600Machine m;
        m.memory().attachSlot1Card(plainRamCard(0x8000));  // 32 KB RAM module in slot 1, folded into S0 at boot
        CHECK(bootPC1600(m));
        auto areas = pc1600BasicAreas(m);
        CHECK(areas.size() >= 2);
        if (areas.size() >= 2) {
            CHECK(areas.front().slot == 1 && areas.front().windowBase == 0x8000);
            CHECK(areas.front().imageOffset == 0);
            CHECK(areas.back().slot == 0);
        }
    }
}

}  // namespace

int run_machine_code_file_tests() {
    test_read_ce158();
    test_read_ffff_autorun_is_none();
    test_read_ce158_length_mismatch();
    test_read_ce158_basic_type_rejected();
    test_read_pc1600();
    test_read_pc1600_basic_type_rejected();
    test_read_basic_listing_rejected();
    test_read_other_kinds_rejected();
    test_read_truncated();
    test_read_leading_noise();
    test_read_headerless();
    test_plan_model_mismatch();
    test_plan_headerless_needs_address();
    test_plan_pc1600_target();
    test_plan_load_configurations();
    test_advice_pc1500();
    test_advice_pc1600();
    test_plan_header_bank();
    test_parse_hex_address();
    test_pc1600_writer();
    test_pc1500_writer();
    test_pc1600_basic_areas();

    std::printf("machine_code_file_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
