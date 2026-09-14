#include "PC1600ProgramPlacement.hpp"

#include <array>
#include <cstdio>

namespace pc1600 {

namespace {

std::string hex(uint16_t v, int digits) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "$%0*X", digits, v);
    return buf;
}

constexpr uint16_t kInternalBase = 0xC000;
constexpr uint16_t kWorkAreaBase = 0xF000;   // internal RAM ends here (fixed work area above)
constexpr uint16_t kSlotWindowTop = 0xBFFF;  // page-2 module window top
constexpr uint32_t kHeaderReserve = 197;     // 8-byte header + 189-byte NEW reserve ($C5)

uint16_t peekBE(const PlacementInput& in, uint16_t addr) {
    return static_cast<uint16_t>((in.peek(addr) << 8) | in.peek(static_cast<uint16_t>(addr + 1)));
}

PlacementResult fail(const std::string& msg) {
    PlacementResult r;
    r.ok = false;
    r.error = msg;
    return r;
}

// Size of what the module presents through the page-2 window at bank 0 --
// its whole image if unbanked, or one bank of a banked one. Falls back to
// the image size if bankSize wasn't filled in.
uint32_t windowContent(const SlotGeometry& g) {
    return g.bankSize ? g.bankSize : g.imageSize;
}

// Top-justified window base: a >=16 KB window fills the whole page-2 window
// ($8000); a smaller module sits at the top of it (CE-155 8 KB -> $A000,
// CE-151 4 KB -> $B000). Doc §1.
uint16_t windowBase(const SlotGeometry& g) {
    uint32_t w = windowContent(g);
    uint32_t inBank = w >= 0x4000 ? 0x4000u : w;
    return static_cast<uint16_t>(kInternalBase - inBank);
}

// Card-image offset of that ADTBL bank's window base, within bank 0 of the
// module (the BASIC program area never leaves bank 0 -- for a vertically
// banked CE-1601M that is Port 28H = 0, the first bankSize bytes of the
// image). A >=32 KB window splits into a low / high 16 KB half picked by
// the ADTBL bank parity (mirroring the PVOUT half-select); a smaller
// window is a single slice at offset 0.
uint32_t bankHalfOffset(const SlotGeometry& g, int adtblBank) {
    if (windowContent(g) > 0x4000) return static_cast<uint32_t>(adtblBank & 1) * 0x4000u;
    return 0;
}

const SlotGeometry& geomForSlot(const PlacementInput& in, int slot) {
    return slot == 2 ? in.slot2 : in.slot1;
}

// Highest usable internal-RAM address: $EFFF, unless VARIABLE POINTER
// ($F899, LH5803-side) names a lower ceiling (variables grow down from the
// top of internal RAM). After NEW0 the variable area is empty and this is
// at / near $EF00.
uint16_t internalCeiling(const PlacementInput& in) {
    uint16_t vp = peekBE(in, kVarPtr);
    if (vp >= 0x4000 && vp < 0x7000) {
        uint16_t z = static_cast<uint16_t>(vp + 0x8000);
        if (z > kInternalBase && z <= kWorkAreaBase)
            return static_cast<uint16_t>(z - 1);
    }
    return static_cast<uint16_t>(kWorkAreaBase - 1);  // $EFFF
}

// Walk the (payload + $FF marker) byte stream across `segments`, emitting
// one contiguous PlacementWrite per segment touched. Fills startAddr /
// endAddr. Errors if the stream runs off the last segment.
bool placeImage(PlacementResult& r, size_t payloadLen) {
    const size_t total = payloadLen + 1;  // + terminating $FF
    const auto& segs = r.segments;

    size_t seg = 0;
    uint32_t addr = segs[0].base;
    r.startAddr = segs[0].base;

    size_t runStartSrc = 0;
    uint32_t runStartAddr = addr;
    size_t runLen = 0;
    auto flush = [&]() {
        if (runLen == 0) return;
        const ProgramSegment& s = segs[seg];
        PlacementWrite w;
        w.kind = s.kind;
        w.slot = s.slot;
        w.backingOffset = s.backingBase + (runStartAddr - s.base);
        w.sourceOffset = runStartSrc;
        w.length = runLen;
        r.writes.push_back(w);
        runLen = 0;
    };

    for (size_t i = 0; i < total; ++i) {
        if (addr > segs[seg].top) {
            flush();
            if (++seg >= segs.size()) {
                r.error = "program too large for the user area";
                return false;
            }
            addr = segs[seg].base;
        }
        if (runLen == 0) {
            runStartAddr = addr;
            runStartSrc = i;
        }
        ++runLen;
        if (i + 1 == total) r.endAddr = static_cast<uint16_t>(addr);
        ++addr;
    }
    flush();
    return true;
}

// Shared builder: the ADTBL slice [firstIdx..lastIdx] (1-based) -> module
// segments. `appendInternal` adds internal RAM as the final segment (S0
// only). `leadingBaseFromBasPrgSt` uses BASPRG_ST for segment 0's base
// (S0: it already reflects the $C5 reserve and any NEW "Sx:" ML hole);
// otherwise segment 0 starts at window_base + 197 (a program-module region).
PlacementResult buildPlacement(const PlacementInput& in, int firstIdx, int lastIdx,
                               bool appendInternal, bool leadingBaseFromBasPrgSt,
                               size_t payloadLen) {
    PlacementResult r;
    r.basPrgStValue = peekBE(in, kBasPrgSt);

    std::array<uint8_t, 6> adtbl{};  // index 1..5 used
    for (int n = 1; n <= 5; ++n)
        adtbl[n] = in.peek(static_cast<uint16_t>(kAdtbl1 + (n - 1)));

    if (firstIdx >= 1 && firstIdx <= 5 && lastIdx >= firstIdx && lastIdx <= 5) {
        for (int n = firstIdx; n <= lastIdx; ++n) {
            uint8_t b = adtbl[n];
            if (b == 0) continue;
            int bank = (b >> 4) & 3;
            int slot = b & 0x0F;
            if (slot != 1 && slot != 2)
                return fail("ADTBL[" + std::to_string(n) + "]=" + hex(b, 2) +
                            " names slot " + std::to_string(slot) + " (expected 1 or 2)");
            const SlotGeometry& g = geomForSlot(in, slot);
            if (!g.present)
                return fail("ADTBL puts the program in slot " + std::to_string(slot) +
                            ", but no module is fitted there");
            if (windowContent(g) < 0x1000)
                return fail("slot " + std::to_string(slot) + " module window is only " +
                            std::to_string(windowContent(g)) + " bytes -- too small for the "
                            "BASIC program area");
            // A vertically / D4 banked module (CE-1601M, ...) is fine: the
            // BASIC program area lives entirely in bank 0 (Port 28H = 0),
            // the first windowContent(g) bytes of the card image.
            ProgramSegment s;
            s.kind = ProgramSegment::Kind::SlotModule;
            s.slot = slot;
            s.adtblBank = bank;
            s.base = windowBase(g);
            s.top = kSlotWindowTop;
            s.backingBase = bankHalfOffset(g, bank);
            r.segments.push_back(s);
        }
    }

    if (appendInternal) {
        ProgramSegment s;
        s.kind = ProgramSegment::Kind::InternalRam;
        s.base = kInternalBase;
        s.top = internalCeiling(in);
        s.backingBase = 0;
        r.segments.push_back(s);
    }

    if (r.segments.empty())
        return fail("no S0 segments -- ADTBL and the internal-RAM fallback are both empty");

    // Segment 0's true start.
    ProgramSegment& seg0 = r.segments.front();
    if (leadingBaseFromBasPrgSt) {
        uint32_t z = static_cast<uint32_t>(r.basPrgStValue) + 0x8000u;
        if (z < seg0.base || z > seg0.top)
            return fail("BASPRG_ST ($F865=" + hex(r.basPrgStValue, 4) +
                        ") lands outside the first S0 segment " + hex(seg0.base, 4) + ".." +
                        hex(seg0.top, 4));
        seg0.backingBase += static_cast<uint16_t>(z) - seg0.base;
        seg0.base = static_cast<uint16_t>(z);
    } else {
        seg0.backingBase += kHeaderReserve;
        seg0.base = static_cast<uint16_t>(seg0.base + kHeaderReserve);
    }

    if (!placeImage(r, payloadLen))
        return r;  // r.error set, r.ok stays false

    r.ok = true;
    return r;
}

}  // namespace

PlacementResult planS0Placement(const PlacementInput& in, size_t payloadLen) {
    int s0mtb = in.peek(kS0MTb);
    int s1mtb = in.peek(kS1MTb);
    int s2mtb = in.peek(kS2MTb);

    PlacementResult r = buildPlacement(in, s0mtb, 5, /*appendInternal=*/true,
                                       /*leadingBaseFromBasPrgSt=*/true, payloadLen);
    r.programModuleCase = (s1mtb >= 1 && s1mtb <= 5) || (s2mtb >= 1 && s2mtb <= 5);
    return r;
}

PlacementResult planModuleRegionPlacement(const PlacementInput& in, int slot, size_t payloadLen) {
    const uint16_t mtbAddr = slot == 2 ? kS2MTb : kS1MTb;
    const uint16_t mbbAddr = slot == 2 ? kS2MBb : kS1MBb;
    int mtb = in.peek(mtbAddr);
    int mbb = in.peek(mbbAddr);
    if (!(mtb >= 1 && mtb <= 5 && mbb >= mtb && mbb <= 5))
        return fail("slot " + std::to_string(slot) + " is not currently a BASIC program module (" +
                    (slot == 2 ? "S2MTb" : "S1MTb") + " is not a 1..5 index)");

    PlacementResult r = buildPlacement(in, mtb, mbb, /*appendInternal=*/false,
                                       /*leadingBaseFromBasPrgSt=*/false, payloadLen);
    r.programModuleCase = true;
    return r;
}

}  // namespace pc1600
