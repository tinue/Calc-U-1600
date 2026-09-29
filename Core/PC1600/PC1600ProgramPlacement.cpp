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
        uint16_t z = lh5803ToZ80(vp);
        if (z > kInternalBase && z <= kWorkAreaBase)
            return static_cast<uint16_t>(z - 1);
    }
    return static_cast<uint16_t>(kWorkAreaBase - 1);  // $EFFF
}

// Lay the program's line records down across `segments` the way the ROM's
// LOAD does (LOADSTORE, rom3b 7074H):
//  - Where one segment runs straight on into the next in Z-80 addresses
//    (ADTBL entry 5's module bank -> internal RAM at $C000), the program
//    simply continues; a line may straddle that seam.
//  - At any other segment end (module bank -> next module bank) lines don't
//    straddle: a line is stored only if 2 bytes stay free after it
//    (addr + record + 2 <= top). Otherwise the ROM writes a two-byte
//    bank-end mark 00 00 and carries on at the next bank's base.
//  - In the last segment the same test decides between storing and "out of
//    memory".
// The $FF end mark follows the last line. Fills writes / startAddr /
// endAddr / endSegment.
bool placeImage(PlacementResult& r, const std::vector<uint8_t>& payload) {
    const auto& segs = r.segments;
    // Split into line records: [lineNo hi][lineNo lo][len][len bytes]. A lone
    // $FF is the end mark between two program segments (a `#SEGMENT` /
    // `99999` line in a listing); LOADLINE (rom3b 6F70H) makes it a 1-byte
    // record that LOADSTORE places like any line. A saved file carries it as
    // FF 00 00 (line 0 can't exist); LOADLINE reads all three, stores the FF.
    std::vector<std::pair<size_t, size_t>> records;  // offset, size
    for (size_t i = 0; i < payload.size();) {
        if (payload[i] == 0xFF) {
            records.push_back({i, 1});
            const bool wire = i + 2 < payload.size() && payload[i + 1] == 0x00 && payload[i + 2] == 0x00;
            i += wire ? 3 : 1;
            continue;
        }
        if (i + 3 > payload.size() || i + 3 + payload[i + 2] > payload.size()) {
            r.error = "the tokenized program is malformed (a line record runs past its end)";
            return false;
        }
        const size_t size = 3u + payload[i + 2];
        records.push_back({i, size});
        i += size;
    }

    size_t seg = 0;
    uint32_t addr = segs[0].base;
    r.startAddr = segs[0].base;

    // Byte output: extend the current write while it stays in one segment.
    auto put = [&](uint8_t byte) {
        while (addr > segs[seg].top) {  // only across a contiguous seam
            ++seg;
        }
        const ProgramSegment& s = segs[seg];
        if (r.writes.empty() || r.writes.back().segment != seg ||
            r.writes.back().backingOffset + r.writes.back().data.size() != s.backingBase + (addr - s.base)) {
            PlacementWrite w;
            w.kind = s.kind;
            w.slot = s.slot;
            w.segment = seg;
            w.backingOffset = s.backingBase + (addr - s.base);
            r.writes.push_back(w);
        }
        r.writes.back().data.push_back(byte);
        ++addr;
    };
    // The last segment of the run the current segment belongs to (contiguous seams merged).
    auto runEnd = [&](size_t k) {
        while (k + 1 < segs.size() && uint32_t(segs[k].top) + 1 == segs[k + 1].base) ++k;
        return k;
    };

    for (const auto& rec : records) {
        for (;;) {
            const size_t last = runEnd(seg);
            if (addr + rec.second + 2 <= segs[last].top) break;
            if (last + 1 >= segs.size()) {
                r.error = "program too large for the user area";
                return false;
            }
            put(0x00);  // bank-end mark
            put(0x00);
            seg = last + 1;
            addr = segs[seg].base;
        }
        for (size_t i = 0; i < rec.second; ++i) put(payload[rec.first + i]);
    }
    if (addr > segs[runEnd(seg)].top) {
        r.error = "program too large for the user area";
        return false;
    }
    while (addr > segs[seg].top) ++seg;
    r.endAddr = static_cast<uint16_t>(addr);
    r.endSegment = seg;
    put(0xFF);
    return true;
}

// Shared builder: the ADTBL slice [firstIdx..lastIdx] (1-based) -> module
// segments. `appendInternal` adds internal RAM as the final segment (S0
// only). `leadingBaseFromBasPrgSt` uses BASPRG_ST for segment 0's base
// (S0: it already reflects the $C5 reserve and any NEW "Sx:" ML hole);
// otherwise segment 0 starts at window_base + 197 (a program-module region).
PlacementResult buildPlacement(const PlacementInput& in, int firstIdx, int lastIdx,
                               bool appendInternal, bool leadingBaseFromBasPrgSt,
                               const std::vector<uint8_t>& payload, uint16_t regionStart = 0,
                               uint8_t regionLimitPage = 0) {
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
            s.adtblIndex = n;
            s.base = windowBase(g);
            s.windowBase = s.base;
            s.top = kSlotWindowTop;
            s.backingBase = bankHalfOffset(g, bank);
            r.segments.push_back(s);
        }
    }

    if (appendInternal) {
        ProgramSegment s;
        s.kind = ProgramSegment::Kind::InternalRam;
        s.base = kInternalBase;
        s.windowBase = kInternalBase;
        s.top = internalCeiling(in);
        s.backingBase = 0;
        r.segments.push_back(s);
    }

    if (r.segments.empty())
        return fail("no S0 segments -- ADTBL and the internal-RAM fallback are both empty");

    // Segment 0's true start.
    ProgramSegment& seg0 = r.segments.front();
    if (leadingBaseFromBasPrgSt) {
        uint32_t z = lh5803ToZ80(r.basPrgStValue);
        if (z < seg0.base || z > seg0.top)
            return fail("BASPRG_ST ($F865=" + hex(r.basPrgStValue, 4) +
                        ") lands outside the first S0 segment " + hex(seg0.base, 4) + ".." +
                        hex(seg0.top, 4));
        seg0.backingBase += static_cast<uint16_t>(z) - seg0.base;
        seg0.base = static_cast<uint16_t>(z);
    } else {
        // A program-module region: the descriptor's start, when it lies in
        // the leading bank's window past the header; else window + 197.
        uint16_t start = static_cast<uint16_t>(seg0.base + kHeaderReserve);
        if (regionStart >= start && regionStart <= seg0.top) start = regionStart;
        seg0.backingBase += static_cast<uint16_t>(start - seg0.base);
        seg0.base = start;
        // The descriptor's limit page ends the last bank (C0H = the whole window).
        ProgramSegment& last = r.segments.back();
        if (regionLimitPage > (last.base >> 8) && regionLimitPage < 0xC0)
            last.top = static_cast<uint16_t>((regionLimitPage << 8) - 1);
    }

    if (!placeImage(r, payload))
        return r;  // r.error set, r.ok stays false

    r.ok = true;
    return r;
}

}  // namespace

PlacementResult planS0Placement(const PlacementInput& in, const std::vector<uint8_t>& payload) {
    int s0mtb = in.peek(kS0MTb);
    int s1mtb = in.peek(kS1MTb);
    int s2mtb = in.peek(kS2MTb);

    PlacementResult r = buildPlacement(in, s0mtb, 5, /*appendInternal=*/true,
                                       /*leadingBaseFromBasPrgSt=*/true, payload);
    r.programModuleCase = (s1mtb >= 1 && s1mtb <= 5) || (s2mtb >= 1 && s2mtb <= 5);
    return r;
}

PlacementResult planModuleRegionPlacement(const PlacementInput& in, int slot, const std::vector<uint8_t>& payload) {
    const uint16_t mtbAddr = slot == 2 ? kS2MTb : kS1MTb;
    const uint16_t mbbAddr = slot == 2 ? kS2MBb : kS1MBb;
    int mtb = in.peek(mtbAddr);
    int mbb = in.peek(mbbAddr);
    if (!(mtb >= 1 && mtb <= 5 && mbb >= mtb && mbb <= 5))
        return fail("slot " + std::to_string(slot) + " is not currently a BASIC program module (" +
                    (slot == 2 ? "S2MTb" : "S1MTb") + " is not a 1..5 index)");

    const uint16_t desc = slot == 2 ? kS2Desc : kS1Desc;
    const uint16_t start = static_cast<uint16_t>(in.peek(static_cast<uint16_t>(desc + 4)) |
                                                 (in.peek(static_cast<uint16_t>(desc + 5)) << 8));
    const uint8_t limitPage = in.peek(static_cast<uint16_t>(desc + 2));
    PlacementResult r = buildPlacement(in, mtb, mbb, /*appendInternal=*/false,
                                       /*leadingBaseFromBasPrgSt=*/false, payload, start, limitPage);
    r.programModuleCase = true;
    return r;
}

}  // namespace pc1600
