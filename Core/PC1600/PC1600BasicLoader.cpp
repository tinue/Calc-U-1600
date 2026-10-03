#include "PC1600BasicLoader.hpp"

#include <cstdio>

#include "../Basic/BasicProgramSource.hpp"
#include "PC1600Machine.hpp"
#include "PC1600MachineCodeLoader.hpp"
#include "PC1600ProgramPlacement.hpp"

namespace {

// BASIC program-area pointers (SC7852 view). F865/F867/F869 (big-endian)
// are the PC-1500's BASPRG_ST/END/EDT trio, inherited verbatim
// (Ref/PC-1600/PC-1600-Work-Area-Map.md, Block C). Their *values* are LH5803-side
// addresses -- $40C5 on a stock machine, $00C5 once a Slot-1/Slot-2 RAM
// module has moved the program area into a parked bank window. See
// Ref/PC-1600/PC-1600-BASIC-Program-Placement.md.
constexpr uint16_t kBasPrgSt = 0xF865;
constexpr uint16_t kBasPrgEnd = 0xF867;
constexpr uint16_t kPrgAdrStart = 0xFE3C;  // start lo, hi, bank; end lo, hi, bank (FE3C-FE41)
constexpr uint16_t kPrgBankStart = 0xF02B; // ADTBL index of the S0 start / end
constexpr uint16_t kPrgBankEnd = 0xF02C;
constexpr uint16_t kRamEnd = 0xF864;       // page of the S0 user-area top
constexpr uint16_t kVarPtr = 0xF899;       // VARIABLE POINTER (BE, stored form)
constexpr uint16_t kCurTop = 0xF89E;       // CURRENT TOP (BE, stored form)
constexpr uint16_t kCurBank = 0xF1C1;      // CURRENT logical bank
constexpr uint16_t kRamBaseLE = 0xF5CF;  // Z-80-native base right after NEW0; stack-region, cross-check only

uint16_t readBE16(PC1600Machine& m, uint16_t addr) {
    return static_cast<uint16_t>((m.debugPeek(addr) << 8) |
                                 m.debugPeek(static_cast<uint16_t>(addr + 1)));
}
uint16_t readLE16(PC1600Machine& m, uint16_t addr) {
    return static_cast<uint16_t>(m.debugPeek(addr) |
                                 (m.debugPeek(static_cast<uint16_t>(addr + 1)) << 8));
}

BasicLoadResult fail(const std::string& msg) {
    BasicLoadResult r;
    r.ok = false;
    r.error = msg;
    return r;
}

bool writeBacking(PC1600Machine& machine, pc1600::ProgramSegment::Kind kind, int slot, uint32_t offset,
                  const uint8_t* src, size_t n) {
    return kind == pc1600::ProgramSegment::Kind::InternalRam ? machine.debugWriteInternalRam(offset, src, n)
                                                             : machine.debugWriteSlotImage(slot, offset, src, n);
}

}  // namespace

basic::TransferModel pc1600ListingModel(PC1600Machine& machine) {
    return machine.mode1() ? basic::TransferModel::PC1500 : basic::TransferModel::PC1600;
}

namespace {

BasicLoadResult loadSource(PC1600Machine& machine, const basic::BasicProgramSource& src) {
    if (!src.ok) {
        std::string error = src.error;
        if (src.listing && machine.mode1())
            error += " (MODE 1: the listing is read as PC-1500 BASIC)";
        return fail(error);
    }
    // The ROM's LOAD takes a PC-1600 program in both MODEs; a PC-1500 one
    // only in MODE 1 (docs/background/plans/Loader-Mode-Plan.md).
    if (!src.listing && src.source == basic::TransferModel::PC1500 && !machine.mode1())
        return fail("this is a PC-1500 (CE-158) tokenized BASIC program -- the PC-1600 takes it in MODE 1 "
                    "only (type MODE1 first)");
    return loadBasicBinaryPayload(machine, src.payload);
}

}  // namespace

BasicLoadResult loadBasicProgram(PC1600Machine& machine, const std::vector<uint8_t>& file) {
    return loadSource(machine, basic::readBasicProgram(file, pc1600ListingModel(machine)));
}

BasicLoadResult loadBasicProgramFile(PC1600Machine& machine, const std::string& path) {
    return loadSource(machine, basic::readBasicProgramFile(path, pc1600ListingModel(machine)));
}

namespace {

void pokeByte(PC1600Machine& m, uint16_t addr, uint8_t v) { m.pokeMemory(addr, &v, 1); }

void pokeBE16(PC1600Machine& m, uint16_t addr, uint16_t v) {
    const uint8_t b[2] = {static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v & 0xFF)};
    m.pokeMemory(addr, b, 2);
}

// The segment of `segs` that holds the program end `end` (Z-80 address)
// with ADTBL index `endIndex`, or -1 -- internal RAM matches by address.
int segmentOf(const std::vector<pc1600::ProgramSegment>& segs, uint16_t end, int endIndex) {
    for (size_t i = 0; i < segs.size(); ++i) {
        const pc1600::ProgramSegment& sg = segs[i];
        if (end >= sg.base && end <= sg.top &&
            (sg.kind == pc1600::ProgramSegment::Kind::InternalRam || sg.adtblIndex == endIndex))
            return static_cast<int>(i);
    }
    return -1;
}

bool writeImage(PC1600Machine& machine, const pc1600::PlacementResult& plan, std::string* error) {
    for (const pc1600::PlacementWrite& w : plan.writes) {
        if (!writeBacking(machine, w.kind, w.slot, w.backingOffset, w.data.data(), w.data.size())) {
            char buf[192];
            if (w.kind == pc1600::ProgramSegment::Kind::InternalRam)
                std::snprintf(buf, sizeof(buf), "backing-store write failed (internal RAM +$%X, %zu bytes)",
                              w.backingOffset, w.data.size());
            else
                std::snprintf(buf, sizeof(buf), "backing-store write failed (slot %d image +$%X, %zu bytes)",
                              w.slot, w.backingOffset, w.data.size());
            *error = buf;
            return false;
        }
    }
    return true;
}

// Erase the resident program -- from the area's start through its end mark,
// bank-end marks included -- so a shorter reload leaves no stale tokens.
void eraseOld(PC1600Machine& machine, const std::vector<pc1600::ProgramSegment>& segs, int endSegment,
              uint16_t end) {
    for (int i = 0; i <= endSegment; ++i) {
        const pc1600::ProgramSegment& sg = segs[i];
        const uint16_t last = i == endSegment ? end : sg.top;
        const std::vector<uint8_t> blank(static_cast<size_t>(last - sg.base) + 1, 0x00);
        if (!writeBacking(machine, sg.kind, sg.slot, sg.backingBase, blank.data(), blank.size()))
            std::fprintf(stderr, "[PC1600BasicLoader] note: failed to clear the old program\n");
    }
}

}  // namespace

// LOAD semantics: works off the live work area -- no reset, no MODE / TITLE
// change, no NEW0. The target is the program area TITLE (F1D5H) selects:
// S0 (placement across S0's ADTBL banks and internal RAM, planS0Placement)
// or the S1 / S2 program module (planModuleRegionPlacement). The bytes go
// straight into the backing store; the pointers are then left exactly as
// the ROM's own LOAD leaves them (LOADEND, rom3b 70E1H --
// Ref/PC-1600/PC-1600-Work-Area-Map.md §3.5 / §4.5).
BasicLoadResult loadBasicBinaryPayload(PC1600Machine& machine,
                                       const std::vector<uint8_t>& payload) {
    const int title = machine.programAreaTitle();
    pc1600::PlacementInput in;
    in.peek = [&machine](uint16_t a) { return machine.debugPeek(a); };
    in.slot1 = pc1600SlotGeometry(machine, 1);
    in.slot2 = pc1600SlotGeometry(machine, 2);

    std::string error;

    if (title == 1 || title == 2) {
        // ── S1 / S2 program module ─────────────────────────────────────
        const uint16_t desc = pc1600::slotDescriptorAddress(title);
        const pc1600::SlotDescriptor oldDesc = pc1600::readSlotDescriptor(in.peek, title);
        pc1600::PlacementResult areaPlan = pc1600::planModuleRegionPlacement(in, title, {});
        if (!areaPlan.ok) return fail("TITLE selects S" + std::to_string(title) + ", but " + areaPlan.error);
        const uint16_t oldEnd = oldDesc.end;
        const int oldEndSegment = segmentOf(areaPlan.segments, oldEnd, oldDesc.endIndex);
        if (oldEndSegment < 0 || (oldEndSegment == 0 && oldEnd < areaPlan.startAddr)) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "the S%d program end ($%04X) is not inside its program area -- run NEW \"S%d:\" first",
                          title, oldEnd, title);
            return fail(buf);
        }
        pc1600::PlacementResult plan = pc1600::planModuleRegionPlacement(in, title, payload);
        if (!plan.ok) return fail(plan.error);
        eraseOld(machine, areaPlan.segments, oldEndSegment, oldEnd);
        if (!writeImage(machine, plan, &error)) return fail(error);

        // End triple (Z-80 page-2 address lo, hi, ADTBL index).
        const pc1600::ProgramSegment& endSeg = plan.segments[plan.endSegment];
        const uint8_t endTriple[3] = {static_cast<uint8_t>(plan.endAddr & 0xFF), static_cast<uint8_t>(plan.endAddr >> 8),
                                      static_cast<uint8_t>(endSeg.adtblIndex)};
        machine.pokeMemory(static_cast<uint16_t>(desc + 7), endTriple, 3);
        // Module header +5/+6: the end as an offset from the base page, high
        // byte first; one bank further on counts from base 4000H (LOADEND 7134H).
        const pc1600::ProgramSegment& seg0 = plan.segments.front();
        const uint16_t from = endSeg.adtblIndex == seg0.adtblIndex ? static_cast<uint16_t>(oldDesc.basePage << 8)
                                                                   : 0x4000;
        const uint16_t offset = static_cast<uint16_t>((plan.endAddr - from) & 0x7FFF);
        const uint8_t hdr[2] = {static_cast<uint8_t>(offset >> 8), static_cast<uint8_t>(offset & 0xFF)};
        const uint32_t headerBacking = seg0.backingBase - (seg0.base - seg0.windowBase);
        if (!machine.debugWriteSlotImage(title, headerBacking + 5, hdr, 2))
            return fail("could not update the S" + std::to_string(title) + " module header");
        // PRGADR copies the descriptor's start/end triples to FE3C-FE41.
        uint8_t prgAdr[6];
        for (int i = 0; i < 6; ++i) prgAdr[i] = machine.debugPeek(static_cast<uint16_t>(desc + 4 + i));
        machine.pokeMemory(kPrgAdrStart, prgAdr, 6);
    } else {
        // ── S0 ───────────────────────────────────────────────────────────
        uint16_t stLh = readBE16(machine, kBasPrgSt);
        if (stLh == 0x0000 || stLh == 0xFFFF || stLh >= 0x7000) {
            char buf[192];
            std::snprintf(buf, sizeof(buf), "BASPRG_ST looks uninitialised ($F865=$%04X) -- run NEW before loading", stLh);
            return fail(buf);
        }
        pc1600::PlacementResult areaPlan = pc1600::planS0Placement(in, {});
        if (!areaPlan.ok) return fail(areaPlan.error);
        const uint16_t endLhOld = readBE16(machine, kBasPrgEnd);
        const uint16_t oldEnd = pc1600::lh5803ToZ80(endLhOld);
        const int oldEndSegment = endLhOld >= 0x8000 ? -1 : segmentOf(areaPlan.segments, oldEnd, machine.debugPeek(kPrgBankEnd));
        if (oldEndSegment < 0 || (oldEndSegment == 0 && oldEnd < areaPlan.startAddr)) {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "BASPRG_END looks invalid ($F867=$%04X) for BASPRG_ST $%04X -- run NEW before loading",
                          endLhOld, stLh);
            return fail(buf);
        }
        pc1600::PlacementResult plan = pc1600::planS0Placement(in, payload);
        if (!plan.ok) return fail(plan.error);
        uint16_t f5cf = readLE16(machine, kRamBaseLE);
        if (f5cf != plan.startAddr)
            std::fprintf(stderr, "[PC1600BasicLoader] note: $F5CF=$%04X != program start $%04X\n", f5cf,
                         plan.startAddr);
        eraseOld(machine, areaPlan.segments, oldEndSegment, oldEnd);
        if (!writeImage(machine, plan, &error)) return fail(error);

        const pc1600::ProgramSegment& endSeg = plan.segments[plan.endSegment];
        pokeBE16(machine, kBasPrgEnd, pc1600::z80ToLh5803(plan.endAddr));
        pokeByte(machine, kPrgBankEnd, static_cast<uint8_t>(endSeg.adtblIndex));
        // The variables must start above the new end (LOGEND 02DAH): else
        // VARIABLE POINTER := RAM_END:00.
        if (endSeg.kind == pc1600::ProgramSegment::Kind::InternalRam &&
            pc1600::lh5803ToZ80(readBE16(machine, kVarPtr)) <= plan.endAddr)
            pokeBE16(machine, kVarPtr, static_cast<uint16_t>(machine.debugPeek(kRamEnd) << 8));
        // PRGADR: the start and end as Z-80 addresses (low byte first) with
        // their ADTBL index each.
        const uint16_t startZ80 = pc1600::lh5803ToZ80(stLh);
        const uint8_t prgAdr[6] = {static_cast<uint8_t>(startZ80 & 0xFF), static_cast<uint8_t>(startZ80 >> 8),
                                   machine.debugPeek(kPrgBankStart), static_cast<uint8_t>(plan.endAddr & 0xFF),
                                   static_cast<uint8_t>(plan.endAddr >> 8), static_cast<uint8_t>(endSeg.adtblIndex)};
        machine.pokeMemory(kPrgAdrStart, prgAdr, 6);
    }

    // PRGRESET (7169H): CURRENT TOP := the start (stored form), CURRENT
    // bank := the start's ADTBL index.
    const uint16_t startZ80 = static_cast<uint16_t>(machine.debugPeek(kPrgAdrStart) |
                                                    (machine.debugPeek(kPrgAdrStart + 1) << 8));
    pokeBE16(machine, kCurTop, pc1600::z80ToLh5803(startZ80));
    pokeByte(machine, kCurBank, machine.debugPeek(kPrgAdrStart + 2));

    BasicLoadResult r;
    r.ok = true;
    r.baseAddr = startZ80;
    r.endAddr = static_cast<uint16_t>(machine.debugPeek(kPrgAdrStart + 3) | (machine.debugPeek(kPrgAdrStart + 4) << 8));
    return r;
}
