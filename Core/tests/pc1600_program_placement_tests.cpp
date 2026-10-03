// Headless C++ tests for Core/PC1600/PC1600ProgramPlacement.cpp -- the
// pure segment-list + scatter-placement logic for the PC-1600 fast BASIC
// loader. Driven entirely by a synthetic `peek` over the work-area bytes
// (no CPU, no ROM), checked against the two worked examples in
// Ref/PC-1600/PC-1600-BASIC-Program-Placement.md.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <map>

#include "../Debug/BasicPointerTable.hpp"
#include "../PC1600/PC1600ProgramPlacement.hpp"

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

using pc1600::PlacementInput;
using pc1600::PlacementResult;
using pc1600::ProgramSegment;
using pc1600::SlotGeometry;

// A tiny writable work-area model: default byte is 0x00, overrides via set().
struct FakeMem {
    std::map<uint16_t, uint8_t> bytes;
    void set(uint16_t a, uint8_t v) { bytes[a] = v; }
    void setBE(uint16_t a, uint16_t v) {
        bytes[a] = static_cast<uint8_t>(v >> 8);
        bytes[static_cast<uint16_t>(a + 1)] = static_cast<uint8_t>(v & 0xFF);
    }
    uint8_t peek(uint16_t a) const {
        auto it = bytes.find(a);
        return it == bytes.end() ? 0x00 : it->second;
    }
};

PlacementInput makeInput(const FakeMem& m) {
    PlacementInput in;
    in.peek = [&m](uint16_t a) { return m.peek(a); };
    return in;
}

// Unbanked module of `size` bytes (bankSize == imageSize).
SlotGeometry ram(uint32_t size) {
    SlotGeometry g;
    g.present = true;
    g.imageSize = size;
    g.bankSize = size;
    return g;
}
// Banked module: `bankCount` banks of `bankSize` bytes, concatenated.
SlotGeometry bankedRam(uint32_t bankSize, uint32_t bankCount) {
    SlotGeometry g;
    g.present = true;
    g.imageSize = bankSize * bankCount;
    g.bankSize = bankSize;
    return g;
}

// `lines` line records of `recordSize` bytes each ([no hi][no lo][len][len
// bytes], len = recordSize - 3, the last content byte the 0D terminator).
std::vector<uint8_t> program(int lines, size_t recordSize) {
    std::vector<uint8_t> p;
    for (int n = 1; n <= lines; ++n) {
        p.push_back(static_cast<uint8_t>(n >> 8));
        p.push_back(static_cast<uint8_t>(n & 0xFF));
        p.push_back(static_cast<uint8_t>(recordSize - 3));
        for (size_t i = 0; i + 4 < recordSize; ++i) p.push_back('A');
        p.push_back(0x0D);
    }
    return p;
}

// ── Stock machine: one internal-RAM segment at $C0C5 ───────────────────
void test_stock_single_internal_segment() {
    FakeMem m;
    m.set(pc1600::kS0MTb, 0xFF);      // not 1..5 -> S0 is internal-only
    m.setBE(pc1600::kBasPrgSt, 0x40C5);
    m.setBE(pc1600::kVarPtr, 0x6F00); // -> ceiling $EEFF
    PlacementInput in = makeInput(m);

    PlacementResult r = pc1600::planS0Placement(in, program(1, 100));
    CHECK(r.ok);
    CHECK(!r.programModuleCase);
    CHECK(r.segments.size() == 1);
    CHECK(r.segments[0].kind == ProgramSegment::Kind::InternalRam);
    CHECK(r.segments[0].base == 0xC0C5);
    CHECK(r.segments[0].top == 0xEEFF);
    CHECK(r.startAddr == 0xC0C5);
    CHECK(r.endAddr == 0xC0C5 + 100);   // the $FF marker
    CHECK(r.writes.size() == 1);
    CHECK(r.writes[0].kind == ProgramSegment::Kind::InternalRam);
    CHECK(r.writes[0].addr == 0xC0C5);
    CHECK(r.writes[0].data.size() == 101);   // payload + $FF
    CHECK(r.writes[0].data.back() == 0xFF);
}

// Two program segments (a `#SEGMENT` / `99999` listing line): the lone $FF
// between them is a 1-byte record, and the second segment restarts its line
// numbers -- rom3b LOADLINE 6F70H.
void test_two_program_segments() {
    FakeMem m;
    m.set(pc1600::kS0MTb, 0xFF);
    m.setBE(pc1600::kBasPrgSt, 0x40C5);
    PlacementInput in = makeInput(m);

    std::vector<uint8_t> p = program(2, 50);
    p.push_back(0xFF);
    const std::vector<uint8_t> second = program(3, 20);
    p.insert(p.end(), second.begin(), second.end());

    PlacementResult r = pc1600::planS0Placement(in, p);
    CHECK(r.ok);
    CHECK(r.writes.size() == 1);
    CHECK(r.writes[0].data.size() == 100 + 1 + 60 + 1);
    CHECK(r.writes[0].data[100] == 0xFF);
    CHECK(r.writes[0].data[101] == 0x00 && r.writes[0].data[102] == 0x01);  // line 1 again
    CHECK(r.writes[0].data.back() == 0xFF);
    CHECK(r.endAddr == 0xC0C5 + 161);

    // The wire form FF 00 00 of a saved file stores the same bytes.
    std::vector<uint8_t> w = program(2, 50);
    w.insert(w.end(), {0xFF, 0x00, 0x00});
    w.insert(w.end(), second.begin(), second.end());
    PlacementResult rw = pc1600::planS0Placement(in, w);
    CHECK(rw.ok);
    CHECK(rw.writes.size() == 1 && rw.writes[0].data == r.writes[0].data);
}

// ── CE-1600M in Slot 1 as extension memory, boot + NEW0 -- the real
//    intro.pc1600 case observed on the emulated machine:
//    S0MTb=$04, ADTBL[4..5] = 01 11, BASPRG_ST=$00C5. ──────────────────
void test_ce1600m_slot1_extension_memory() {
    FakeMem m;
    m.set(pc1600::kS0MTb, 0x04);
    m.set(pc1600::kS1MTb, 0xFE);
    m.set(pc1600::kS2MTb, 0xFF);
    m.set(pc1600::kAdtbl1 + 3, 0x01);   // ADTBL[4] = bank 0 / slot 1
    m.set(pc1600::kAdtbl1 + 4, 0x11);   // ADTBL[5] = bank 1 / slot 1
    m.setBE(pc1600::kBasPrgSt, 0x00C5);
    PlacementInput in = makeInput(m);
    in.slot1 = ram(0x8000);             // CE-1600M: 32 KB, unbanked

    PlacementResult r = pc1600::planS0Placement(in, program(2, 211));
    CHECK(r.ok);
    CHECK(!r.programModuleCase);
    CHECK(r.segments.size() == 3);
    CHECK(r.segments[0].kind == ProgramSegment::Kind::SlotModule);
    CHECK(r.segments[0].slot == 1 && r.segments[0].adtblBank == 0);
    CHECK(r.segments[0].base == 0x80C5 && r.segments[0].top == 0xBFFF);
    CHECK(r.segments[1].slot == 1 && r.segments[1].adtblBank == 1);
    CHECK(r.segments[1].base == 0x8000);
    CHECK(r.segments[2].kind == ProgramSegment::Kind::InternalRam);
    CHECK(r.startAddr == 0x80C5);
    CHECK(r.endAddr == 0x80C5 + 422);
    CHECK(r.writes.size() == 1);
    CHECK(r.writes[0].slot == 1 && r.writes[0].bank == 0);
    CHECK(r.writes[0].addr == 0x80C5);
    CHECK(r.writes[0].data.size() == 423);
}

// ── TRM §3.12.2 Example 1: CE-159 (8 KB) Slot 1 + CE-1600M (32 KB) Slot 2,
//    both extension memory. S0MTb=3, ADTBL[3..5] = 01 22 32
//    -> S0 banks [0, 2, 3] then internal. No line straddles two module
//    banks: the line that doesn't fit leaves a 00 00 bank-end mark and
//    starts the next bank (rom3b LOADSTORE 7074H). ─────────────────────
void test_trm_example1_scatter_and_bank_end_mark() {
    FakeMem m;
    m.set(pc1600::kS0MTb, 3);
    m.set(pc1600::kS1MTb, 0xFE);
    m.set(pc1600::kS2MTb, 0xFE);
    m.set(pc1600::kAdtbl1 + 2, 0x01);   // ADTBL[3] = bank 0 / slot 1
    m.set(pc1600::kAdtbl1 + 3, 0x22);   // ADTBL[4] = bank 2 / slot 2
    m.set(pc1600::kAdtbl1 + 4, 0x32);   // ADTBL[5] = bank 3 / slot 2
    m.setBE(pc1600::kBasPrgSt, 0x20C5); // -> Z-80 $A0C5 (CE-159 window base $A000 + $C5)
    PlacementInput in = makeInput(m);
    in.slot1 = ram(0x2000);             // CE-159: 8 KB, top-justified at $A000
    in.slot2 = ram(0x8000);             // CE-1600M: 32 KB

    // 100-byte lines: 79 fit in $A0C5-$BFFF (a line needs 2 bytes spare
    // after it: addr + 100 + 2 <= $BFFF), the 80th and 81st go to the next bank.
    PlacementResult r = pc1600::planS0Placement(in, program(81, 100));
    CHECK(r.ok);
    CHECK(r.segments.size() == 4);
    CHECK(r.segments[0].slot == 1 && r.segments[0].adtblBank == 0 && r.segments[0].base == 0xA0C5);
    CHECK(r.segments[0].adtblIndex == 3);
    CHECK(r.segments[1].slot == 2 && r.segments[1].adtblBank == 2 && r.segments[1].base == 0x8000);
    CHECK(r.segments[1].adtblIndex == 4);
    CHECK(r.segments[2].slot == 2 && r.segments[2].adtblBank == 3 && r.segments[2].base == 0x8000);
    CHECK(r.segments[3].kind == ProgramSegment::Kind::InternalRam && r.segments[3].adtblIndex == 5);

    CHECK(r.writes.size() == 2);
    CHECK(r.writes[0].slot == 1 && r.writes[0].bank == 0 && r.writes[0].addr == 0xA0C5);
    CHECK(r.writes[0].data.size() == 7900 + 2);          // 79 lines + the 00 00 mark
    CHECK(r.writes[0].data[7900] == 0x00 && r.writes[0].data[7901] == 0x00);
    CHECK(r.writes[1].slot == 2 && r.writes[1].bank == 2 && r.writes[1].addr == 0x8000);
    CHECK(r.writes[1].data.size() == 200 + 1);           // lines 80, 81 + $FF
    CHECK(r.endAddr == 0x8000 + 200);
    CHECK(r.endSegment == 1);

    // A segment $FF after line 79 still fits ($BF81 + 1 + 2 <= $BFFF): it
    // stays in the CE-159 bank, then the 00 00 mark, then the next lines.
    std::vector<uint8_t> p = program(79, 100);
    p.push_back(0xFF);
    const std::vector<uint8_t> second = program(2, 100);
    p.insert(p.end(), second.begin(), second.end());
    PlacementResult s = pc1600::planS0Placement(in, p);
    CHECK(s.ok);
    CHECK(s.writes.size() == 2);
    CHECK(s.writes[0].data.size() == 7900 + 1 + 2);
    CHECK(s.writes[0].data[7900] == 0xFF);
    CHECK(s.writes[0].data[7901] == 0x00 && s.writes[0].data[7902] == 0x00);
    CHECK(s.writes[1].data.size() == 200 + 1);
}

// ADTBL entry 5's module bank runs straight on into internal RAM ($BFFF ->
// $C000), so there a line does straddle, with no mark.
void test_entry5_straddles_into_internal_ram() {
    FakeMem m;
    m.set(pc1600::kS0MTb, 5);
    m.set(pc1600::kAdtbl1 + 4, 0x01);   // ADTBL[5] = bank 0 / slot 1
    m.setBE(pc1600::kBasPrgSt, 0x3FC5); // -> Z-80 $BFC5, 59 bytes before $C000
    PlacementInput in = makeInput(m);
    in.slot1 = ram(0x4000);

    PlacementResult r = pc1600::planS0Placement(in, program(2, 100));
    CHECK(r.ok);
    CHECK(r.segments.size() == 2);
    CHECK(r.writes.size() == 2);
    CHECK(r.writes[0].slot == 1 && r.writes[0].addr == 0xBFC5 && r.writes[0].data.size() == 59);
    CHECK(r.writes[1].kind == ProgramSegment::Kind::InternalRam && r.writes[1].addr == 0xC000);
    CHECK(r.writes[1].data.size() == 200 - 59 + 1);
    CHECK(r.endAddr == 0xC000 + 200 - 59);
    CHECK(r.endSegment == 1);
}

// ── TRM §3.12.2 Example 2: CE-1600M Slot 1 + CE-161 Slot 2 as PROGRAM
//    modules. ADTBL = 00 81 11 A2 00, S1MTb/S1MBb = 02/03. The Slot-1
//    region's leading bank gets the 197-byte header+reserve. ───────────
void test_trm_example2_program_module_region() {
    FakeMem m;
    m.set(pc1600::kS0MTb, 5);           // 00 -> S0 internal-only
    m.set(pc1600::kS1MTb, 2);
    m.set(pc1600::kS1MBb, 3);
    m.set(pc1600::kS2MTb, 4);
    m.set(pc1600::kS2MBb, 4);
    m.set(pc1600::kAdtbl1 + 1, 0x81);   // ADTBL[2] = bank 0 / slot 1, leading
    m.set(pc1600::kAdtbl1 + 2, 0x11);   // ADTBL[3] = bank 1 / slot 1
    m.set(pc1600::kAdtbl1 + 3, 0xA2);   // ADTBL[4] = bank 2 / slot 2, leading
    m.setBE(pc1600::kBasPrgSt, 0x40C5);  // the active S0 program still sits in internal RAM
    PlacementInput in = makeInput(m);
    in.slot1 = ram(0x8000);
    in.slot2 = ram(0x4000);

    PlacementResult r = pc1600::planModuleRegionPlacement(in, 1, program(1, 50));
    CHECK(r.ok);
    CHECK(r.programModuleCase);
    CHECK(r.segments.size() == 2);      // no internal-RAM tail for a module region
    CHECK(r.segments[0].slot == 1 && r.segments[0].adtblBank == 0);
    CHECK(r.segments[0].base == 0x80C5);           // window base $8000 + 197
    CHECK(r.segments[1].slot == 1 && r.segments[1].adtblBank == 1);
    CHECK(r.segments[1].base == 0x8000);
    CHECK(r.startAddr == 0x80C5);
    CHECK(r.writes.size() == 1 && r.writes[0].data.size() == 51);

    // planS0Placement on the same machine still drives S0 (internal), but
    // flags the program-module case.
    PlacementResult s0 = pc1600::planS0Placement(in, program(1, 10));
    CHECK(s0.ok);
    CHECK(s0.programModuleCase);
    CHECK(s0.segments.size() == 1 && s0.segments[0].kind == ProgramSegment::Kind::InternalRam);

    // A slot that isn't a program module is rejected by the region entry point.
    PlacementResult bad = pc1600::planModuleRegionPlacement(makeInput(FakeMem{}), 2, program(1, 10));
    CHECK(!bad.ok && bad.error.find("not currently a BASIC program module") != std::string::npos);
}

// ── Error paths ───────────────────────────────────────────────────────
void test_program_too_large() {
    FakeMem m;
    m.set(pc1600::kS0MTb, 0xFF);
    m.setBE(pc1600::kBasPrgSt, 0x40C5);
    PlacementInput in = makeInput(m);
    PlacementResult r = pc1600::planS0Placement(in, program(700, 100));
    CHECK(!r.ok);
    CHECK(r.error.find("too large") != std::string::npos);
}

void test_basprgst_outside_first_segment() {
    FakeMem m;
    m.set(pc1600::kS0MTb, 0xFF);
    m.setBE(pc1600::kBasPrgSt, 0x0100);  // -> Z-80 $8100, not in the internal segment
    PlacementInput in = makeInput(m);
    PlacementResult r = pc1600::planS0Placement(in, program(1, 10));
    CHECK(!r.ok);
    CHECK(r.error.find("outside the first S0 segment") != std::string::npos);
}

void test_adtbl_points_at_absent_module() {
    FakeMem m;
    m.set(pc1600::kS0MTb, 5);
    m.set(pc1600::kAdtbl1 + 4, 0x21);    // ADTBL[5] = bank 2 / slot 2, but slot 2 empty
    PlacementInput in = makeInput(m);
    PlacementResult r = pc1600::planS0Placement(in, program(1, 10));
    CHECK(!r.ok);
    CHECK(r.error.find("no module is fitted") != std::string::npos);
}

// A vertically banked CE-1601M (2 x 32 KB, Port 28H select) used as S0
// extension memory: the program area lives in vertical bank 0, so the
// writes land in the first 32 KB of the card image exactly as for a plain
// CE-1600M. Not rejected.
void test_vertical_banked_module_uses_bank0() {
    FakeMem m;
    m.set(pc1600::kS0MTb, 4);
    m.set(pc1600::kS2MTb, 0xFE);
    m.set(pc1600::kAdtbl1 + 3, 0x22);    // ADTBL[4] = bank 2 / slot 2
    m.set(pc1600::kAdtbl1 + 4, 0x32);    // ADTBL[5] = bank 3 / slot 2
    m.setBE(pc1600::kBasPrgSt, 0x00C5);
    PlacementInput in = makeInput(m);
    in.slot2 = bankedRam(0x8000, 2);     // CE-1601M: 64 KB image, 32 KB per vertical bank

    PlacementResult r = pc1600::planS0Placement(in, program(3, 100));
    CHECK(r.ok);
    CHECK(r.segments.size() == 3);
    CHECK(r.segments[0].slot == 2 && r.segments[0].base == 0x80C5);
    CHECK(r.segments[1].slot == 2 && r.segments[1].base == 0x8000 &&
          r.segments[1].adtblBank == 3);   // high 16 KB half of vertical bank 0
    CHECK(r.segments[2].kind == ProgramSegment::Kind::InternalRam);
    CHECK(r.writes.size() == 1);
    CHECK(r.writes[0].slot == 2 && r.writes[0].bank == 2);
    CHECK(r.writes[0].addr == 0x80C5);
    CHECK(r.writes[0].data.size() == 301);
}

// A module whose window is absurdly small is still rejected.
void test_tiny_module_window_rejected() {
    FakeMem m;
    m.set(pc1600::kS0MTb, 5);
    m.set(pc1600::kAdtbl1 + 4, 0x11);    // ADTBL[5] = bank 1 / slot 1
    PlacementInput in = makeInput(m);
    in.slot1 = ram(0x200);
    PlacementResult r = pc1600::planS0Placement(in, program(1, 10));
    CHECK(!r.ok);
    CHECK(r.error.find("too small") != std::string::npos);
}

// The Debug panel's MEM / STATUS 259 figures, checked against a real unit:
// CE-1600M in slot 1 after INIT"S1:","P" -- MEM 10810, STATUS 259 32571.
void test_debug_program_areas_match_rom() {
    FakeMem m;
    m.set(0xF1D5, 0x01);            // TITLE "S1:"
    m.set(0xF02B, 0x05);
    m.set(0xF02C, 0x05);
    m.set(0xF864, 0x6B);            // RAM_END page
    m.setBE(0xF867, 0x40C5);        // empty S0 program (Z-80 C0C5)
    // S1 descriptor: base 80H, ADTBL 1..2, limit C000H in the second bank,
    // start = end = 80C5H (empty).
    const uint8_t s1[10] = {0x80, 0x01, 0xC0, 0x02, 0xC5, 0x80, 0x01, 0xC5, 0x80, 0x01};
    for (int i = 0; i < 10; ++i) m.set(static_cast<uint16_t>(0xF015 + i), s1[i]);
    m.set(0xF020, 0xFF);            // S2: no program module
    CoreDebug::PC1600ProgramAreas a =
        CoreDebug::readPC1600ProgramAreas([&m](uint16_t addr) { return m.peek(addr); });
    CHECK(a.title == 1);
    CHECK(a.memS0 == 10810);
    CHECK(a.slot[0].programModule());
    CHECK(a.slot[0].freeBytes == 32571);
    CHECK(!a.slot[1].programModule());
    CHECK(pc1600::lh5803ToZ80(0x40C5) == 0xC0C5);
    CHECK(pc1600::lh5803ToZ80(0x6B00) == 0xEB00);
    CHECK(pc1600::lh5803ToZ80(0x00C5) == 0x80C5);
    CHECK(pc1600::z80ToLh5803(0xC0C5) == 0x40C5);
}

}  // namespace

int run_pc1600_program_placement_tests() {
    test_debug_program_areas_match_rom();
    test_stock_single_internal_segment();
    test_two_program_segments();
    test_ce1600m_slot1_extension_memory();
    test_trm_example1_scatter_and_bank_end_mark();
    test_entry5_straddles_into_internal_ram();
    test_trm_example2_program_module_region();
    test_program_too_large();
    test_basprgst_outside_first_segment();
    test_adtbl_points_at_absent_module();
    test_vertical_banked_module_uses_bank0();
    test_tiny_module_window_rejected();

    std::printf("pc1600_program_placement_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
