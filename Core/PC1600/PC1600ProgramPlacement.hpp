#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ── Where a tokenised BASIC program's bytes land in PC-1600 user RAM ────
//
// On a stock PC-1600 the BASIC program area ("S0") is a single contiguous
// window at Z-80 $C0C5 (internal RAM). With a RAM / program module fitted,
// the ROM builds S0 by SCATTERING it across the module's 16 KB banks and
// then internal RAM -- an image larger than one bank straddles a bank
// boundary mid-line. The fast loader has to reproduce that placement to
// inject the program without driving the firmware LOAD path.
//
// This module is the pure-logic half: given a way to read the work-area
// bytes (a `peek` into the SC7852 address space) and the geometry of any
// fitted module, it produces the ordered segment list and the set of
// backing-store writes that place a `payloadLen`-byte tokenised image plus
// its single terminating $FF marker.
//
// Reference: SharpPC1500Reference/PC-1600/PC-1600-BASIC-Program-Placement.md
// (memory model §1, work-area bytes §3, segment list §4, placement §5).
// The address-space geometry and the reserve conventions are from the
// PC-1600 Technical Reference Manual; the ADTBL byte bit-layout is
// reverse-engineered there from two worked examples.

namespace pc1600 {

// ── Work-area byte addresses (SC7852 view; the values they hold are
//    LH5803-side, i.e. Z-80 address - $8000). ───────────────────────────
constexpr uint16_t kS0MTb = 0xF02A;    // 1-based ADTBL index where S0's bank list starts
constexpr uint16_t kS1MTb = 0xF016;    // first / last ADTBL index of a Slot-1 PROGRAM module
constexpr uint16_t kS1MBb = 0xF018;
constexpr uint16_t kS2MTb = 0xF020;    // ... Slot 2
constexpr uint16_t kS2MBb = 0xF022;
constexpr uint16_t kAdtbl1 = 0xF1D6;   // ADTBL+1 .. ADTBL+5 -- five 1-byte bank descriptors
constexpr uint16_t kBasPrgSt = 0xF865; // BASPRG_ST (big-endian), LH5803-side
constexpr uint16_t kVarPtr = 0xF899;   // VARIABLE POINTER (big-endian), LH5803-side

// One contiguous run of the user area, ascending Z-80 (SC7852) address
// order. `base`/`top` are inclusive Z-80 addresses.
struct ProgramSegment {
    enum class Kind { SlotModule, InternalRam };
    Kind kind = Kind::InternalRam;
    int slot = 0;         // 1 or 2 for SlotModule; 0 for InternalRam
    int adtblBank = 0;    // ADTBL global bank 0..3 (SlotModule only)
    uint16_t base = 0;
    uint16_t top = 0;
    // Start of the window this segment lives in ($C000 internal RAM, the
    // module's window base otherwise), before segment 0 is moved up to
    // BASPRG_ST -- where a `NEW "S0:",<size>` reserve counts from.
    uint16_t windowBase = 0;
    // Backing-store offset of `base`: into debugSlotImage(slot) for a
    // SlotModule, or (base - $C000) into internal RAM.
    uint32_t backingBase = 0;
    uint32_t bytes() const { return uint32_t(top) - base + 1; }
};

// Geometry of a fitted module, derived from its attached card.
struct SlotGeometry {
    bool present = false;
    uint32_t imageSize = 0;  // debugSlotImage(slot).size() -- every bank concatenated
    // Size of what sits behind the $8000-$BFFF window at the module's
    // quiescent bank state (vertical bank 0 / bank 0 -- Port 28H = 0):
    // debugImage().size() / bankCount for a banked module, imageSize for an
    // unbanked one. The BASIC program area only ever touches bank 0, which
    // is the first `bankSize` bytes of debugImage(). A >=32 KB window is
    // PVOUT half-split ($8000 low / $A000+PVOUT... i.e. two 16 KB halves);
    // a 16 KB window is a single slice; 8 KB / 4 KB modules top-justify.
    uint32_t bankSize = 0;
};

struct PlacementInput {
    std::function<uint8_t(uint16_t)> peek;  // e.g. PC1600Machine::debugPeek
    SlotGeometry slot1;
    SlotGeometry slot2;
};

// One run of the (payload + $FF) byte stream to copy into one segment's
// backing store. `sourceOffset` indexes the caller's buffer of
// `payloadLen + 1` bytes (the tokenised image followed by the $FF marker).
struct PlacementWrite {
    ProgramSegment::Kind kind = ProgramSegment::Kind::InternalRam;
    int slot = 0;
    uint32_t backingOffset = 0;
    size_t sourceOffset = 0;
    size_t length = 0;
};

struct PlacementResult {
    bool ok = false;
    std::string error;
    std::vector<ProgramSegment> segments;
    std::vector<PlacementWrite> writes;  // program body + the single $FF marker
    uint16_t startAddr = 0;             // Z-80 address of the first program byte
    uint16_t endAddr = 0;              // Z-80 address of the $FF marker
    uint16_t basPrgStValue = 0;        // raw BASPRG_ST ($F865) value, LH5803-side
    bool programModuleCase = false;    // S1MTb / S2MTb is a valid 1..5 index (informational)
};

/// Plan the S0 placement for a `payloadLen`-byte tokenised image (the
/// terminating $FF is accounted for on top). Reads S0MTb / ADTBL /
/// BASPRG_ST / VARIABLE POINTER via `in.peek`. Safe to call twice against the
/// same `PlacementInput` with different lengths (e.g. once with the resident
/// program's length to find which physical segments to erase, once with the
/// new program's length for the real write plan) -- module/bank geometry
/// never changes between the two calls, only how far the segment walk goes.
PlacementResult planS0Placement(const PlacementInput& in, size_t payloadLen);

/// Plan placement into an independent PROGRAM-module region (doc §2): the
/// ADTBL slice [xMTb..xMBb] for `slot`, whose leading bank carries its own
/// 8-byte header + 189-byte reserve (usable base = window_base + 197) and
/// which does NOT continue into internal RAM. Errors if that slot is not
/// currently a program module. Implemented and tested for completeness;
/// the preset loader drives planS0Placement().
PlacementResult planModuleRegionPlacement(const PlacementInput& in, int slot, size_t payloadLen);

}  // namespace pc1600
