// Headless C++ tests for Core/PC1600/PC1600ProgramPlacement.cpp -- the
// pure segment-list + scatter-placement logic for the PC-1600 fast BASIC
// loader. Driven entirely by a synthetic `peek` over the work-area bytes
// (no CPU, no ROM), checked against the two worked examples in
// SharpPC1500Reference/PC-1600/PC-1600-BASIC-Program-Placement.md.
//
// Build & run: see tools/run_tests.sh

#include <cstdint>
#include <cstdio>
#include <map>

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

// ── Stock machine: one internal-RAM segment at $C0C5 ───────────────────
void test_stock_single_internal_segment() {
    FakeMem m;
    m.set(pc1600::kS0MTb, 0xFF);      // not 1..5 -> S0 is internal-only
    m.setBE(pc1600::kBasPrgSt, 0x40C5);
    m.setBE(pc1600::kVarPtr, 0x6F00); // -> ceiling $EEFF
    PlacementInput in = makeInput(m);

    PlacementResult r = pc1600::planS0Placement(in, 100);
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
    CHECK(r.writes[0].backingOffset == 0x00C5);
    CHECK(r.writes[0].sourceOffset == 0);
    CHECK(r.writes[0].length == 101);   // payload + marker
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

    PlacementResult r = pc1600::planS0Placement(in, 422);
    CHECK(r.ok);
    CHECK(!r.programModuleCase);
    CHECK(r.segments.size() == 3);
    CHECK(r.segments[0].kind == ProgramSegment::Kind::SlotModule);
    CHECK(r.segments[0].slot == 1 && r.segments[0].adtblBank == 0);
    CHECK(r.segments[0].base == 0x80C5 && r.segments[0].top == 0xBFFF);
    CHECK(r.segments[1].slot == 1 && r.segments[1].adtblBank == 1);
    CHECK(r.segments[1].base == 0x8000 && r.segments[1].backingBase == 0x4000);
    CHECK(r.segments[2].kind == ProgramSegment::Kind::InternalRam);
    CHECK(r.startAddr == 0x80C5);
    CHECK(r.endAddr == 0x80C5 + 422);
    CHECK(r.writes.size() == 1);
    CHECK(r.writes[0].slot == 1);
    CHECK(r.writes[0].backingOffset == 0x00C5);
    CHECK(r.writes[0].length == 423);
}

// ── TRM §3.12.2 Example 1: CE-159 (8 KB) Slot 1 + CE-1600M (32 KB) Slot 2,
//    both extension memory. S0MTb=3, ADTBL[3..5] = 01 22 32
//    -> S0 banks [0, 2, 3] then internal. A payload that straddles the
//    seg0 -> seg1 boundary splits into two writes. ─────────────────────
void test_trm_example1_scatter_and_straddle() {
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

    // seg0 capacity = $BFFF - $A0C5 + 1 = 8027 bytes.
    const size_t kSeg0Cap = 0xBFFF - 0xA0C5 + 1;
    PlacementResult r = pc1600::planS0Placement(in, 8100);
    CHECK(r.ok);
    CHECK(r.segments.size() == 4);
    CHECK(r.segments[0].slot == 1 && r.segments[0].adtblBank == 0 && r.segments[0].base == 0xA0C5);
    CHECK(r.segments[1].slot == 2 && r.segments[1].adtblBank == 2 && r.segments[1].base == 0x8000);
    CHECK(r.segments[1].backingBase == 0x0000);
    CHECK(r.segments[2].slot == 2 && r.segments[2].adtblBank == 3 && r.segments[2].base == 0x8000);
    CHECK(r.segments[2].backingBase == 0x4000);
    CHECK(r.segments[3].kind == ProgramSegment::Kind::InternalRam);

    CHECK(r.writes.size() == 2);
    CHECK(r.writes[0].slot == 1 && r.writes[0].backingOffset == 0x00C5);
    CHECK(r.writes[0].sourceOffset == 0 && r.writes[0].length == kSeg0Cap);
    CHECK(r.writes[1].slot == 2 && r.writes[1].backingOffset == 0x0000);
    CHECK(r.writes[1].sourceOffset == kSeg0Cap);
    CHECK(r.writes[1].length == (8100 + 1) - kSeg0Cap);
    CHECK(r.endAddr == 0x8000 + ((8100 + 1) - kSeg0Cap) - 1);
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

    PlacementResult r = pc1600::planModuleRegionPlacement(in, 1, 50);
    CHECK(r.ok);
    CHECK(r.programModuleCase);
    CHECK(r.segments.size() == 2);      // no internal-RAM tail for a module region
    CHECK(r.segments[0].slot == 1 && r.segments[0].adtblBank == 0);
    CHECK(r.segments[0].base == 0x80C5);           // window base $8000 + 197
    CHECK(r.segments[0].backingBase == 197);
    CHECK(r.segments[1].slot == 1 && r.segments[1].adtblBank == 1);
    CHECK(r.segments[1].base == 0x8000 && r.segments[1].backingBase == 0x4000);
    CHECK(r.startAddr == 0x80C5);
    CHECK(r.writes.size() == 1 && r.writes[0].length == 51);

    // planS0Placement on the same machine still drives S0 (internal), but
    // flags the program-module case.
    PlacementResult s0 = pc1600::planS0Placement(in, 10);
    CHECK(s0.ok);
    CHECK(s0.programModuleCase);
    CHECK(s0.segments.size() == 1 && s0.segments[0].kind == ProgramSegment::Kind::InternalRam);

    // A slot that isn't a program module is rejected by the region entry point.
    PlacementResult bad = pc1600::planModuleRegionPlacement(makeInput(FakeMem{}), 2, 10);
    CHECK(!bad.ok && bad.error.find("not currently a BASIC program module") != std::string::npos);
}

// ── Error paths ───────────────────────────────────────────────────────
void test_program_too_large() {
    FakeMem m;
    m.set(pc1600::kS0MTb, 0xFF);
    m.setBE(pc1600::kBasPrgSt, 0x40C5);
    PlacementInput in = makeInput(m);
    PlacementResult r = pc1600::planS0Placement(in, 0x10000);
    CHECK(!r.ok);
    CHECK(r.error.find("too large") != std::string::npos);
}

void test_basprgst_outside_first_segment() {
    FakeMem m;
    m.set(pc1600::kS0MTb, 0xFF);
    m.setBE(pc1600::kBasPrgSt, 0x0100);  // -> Z-80 $8100, not in the internal segment
    PlacementInput in = makeInput(m);
    PlacementResult r = pc1600::planS0Placement(in, 10);
    CHECK(!r.ok);
    CHECK(r.error.find("outside the first S0 segment") != std::string::npos);
}

void test_adtbl_points_at_absent_module() {
    FakeMem m;
    m.set(pc1600::kS0MTb, 5);
    m.set(pc1600::kAdtbl1 + 4, 0x21);    // ADTBL[5] = bank 2 / slot 2, but slot 2 empty
    PlacementInput in = makeInput(m);
    PlacementResult r = pc1600::planS0Placement(in, 10);
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

    PlacementResult r = pc1600::planS0Placement(in, 300);
    CHECK(r.ok);
    CHECK(r.segments.size() == 3);
    CHECK(r.segments[0].slot == 2 && r.segments[0].base == 0x80C5);
    CHECK(r.segments[1].slot == 2 && r.segments[1].base == 0x8000 &&
          r.segments[1].backingBase == 0x4000);   // high 16 KB half of vertical bank 0
    CHECK(r.segments[2].kind == ProgramSegment::Kind::InternalRam);
    CHECK(r.writes.size() == 1);
    CHECK(r.writes[0].slot == 2);
    CHECK(r.writes[0].backingOffset == 0x00C5);    // within vertical bank 0 (image < $8000)
    CHECK(r.writes[0].length == 301);
}

// A module whose window is absurdly small is still rejected.
void test_tiny_module_window_rejected() {
    FakeMem m;
    m.set(pc1600::kS0MTb, 5);
    m.set(pc1600::kAdtbl1 + 4, 0x11);    // ADTBL[5] = bank 1 / slot 1
    PlacementInput in = makeInput(m);
    in.slot1 = ram(0x200);
    PlacementResult r = pc1600::planS0Placement(in, 10);
    CHECK(!r.ok);
    CHECK(r.error.find("too small") != std::string::npos);
}

}  // namespace

int run_pc1600_program_placement_tests() {
    test_stock_single_internal_segment();
    test_ce1600m_slot1_extension_memory();
    test_trm_example1_scatter_and_straddle();
    test_trm_example2_program_module_region();
    test_program_too_large();
    test_basprgst_outside_first_segment();
    test_adtbl_points_at_absent_module();
    test_vertical_banked_module_uses_bank0();
    test_tiny_module_window_rejected();

    std::printf("pc1600_program_placement_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail;
}
