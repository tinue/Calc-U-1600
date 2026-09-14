#include "PC1600BasicLoader.hpp"

#include <cstdio>

#include "../Basic/BasicBinaryImage.hpp"
#include "PC1600Machine.hpp"
#include "PC1600ProgramPlacement.hpp"

namespace {

// BASIC program-area pointers (SC7852 view). F865/F867/F869 (big-endian)
// are the PC-1500's BASPRG_ST/END/EDT trio, inherited verbatim
// (PC-1600-Work-Area-Map.md, Block C). Their *values* are LH5803-side
// addresses -- $40C5 on a stock machine, $00C5 once a Slot-1/Slot-2 RAM
// module has moved the program area into a parked bank window. See
// SharpPC1500Reference/PC-1600/PC-1600-BASIC-Program-Placement.md.
constexpr uint16_t kBasPrgSt = 0xF865;
constexpr uint16_t kBasPrgEnd = 0xF867;
constexpr uint16_t kRamBaseLE = 0xF5CF;  // Z-80-native base right after NEW0; stack-region, cross-check only

uint16_t readBE16(PC1600Machine& m, uint16_t addr) {
    return static_cast<uint16_t>((m.debugPeek(addr) << 8) |
                                 m.debugPeek(static_cast<uint16_t>(addr + 1)));
}
uint16_t readLE16(PC1600Machine& m, uint16_t addr) {
    return static_cast<uint16_t>(m.debugPeek(addr) |
                                 (m.debugPeek(static_cast<uint16_t>(addr + 1)) << 8));
}

PC1600BasicLoadResult fail(const std::string& msg) {
    PC1600BasicLoadResult r;
    r.ok = false;
    r.error = msg;
    return r;
}

pc1600::SlotGeometry slotGeometry(PC1600Machine& machine, int slot) {
    pc1600::SlotGeometry g;
    std::vector<uint8_t> img = machine.debugSlotImage(slot);
    g.present = !img.empty();
    g.imageSize = static_cast<uint32_t>(img.size());
    // The BASIC program area only ever occupies bank 0 of the module (a
    // vertically banked CE-1601M rests at Port 28H = 0). For a banked card
    // that is debugImage().size() / bankCount; for an unbanked one it is
    // the whole image.
    PC1600Machine::DebugBankState bs = machine.debugBankState();
    int bankCount = (slot == 2) ? bs.slot2CardBankCount : bs.slot1CardBankCount;
    g.bankSize = (bankCount > 0) ? g.imageSize / static_cast<uint32_t>(bankCount) : g.imageSize;
    return g;
}

}  // namespace

PC1600BasicLoadResult loadBasicBinaryProgram(PC1600Machine& machine,
                                             const std::vector<uint8_t>& transferFile) {
    basic::BasicBinaryImage img = basic::parseBasicBinaryTransfer(transferFile);
    if (!img.ok) return fail(img.error);
    if (img.model != basic::TransferModel::PC1600) {
        return fail("this is a PC-1500 (CE-158) tokenized-BASIC transfer file -- load it in a "
                    "PC-1500 preset");
    }
    return loadBasicBinaryPayload(machine, img.payload);
}

PC1600BasicLoadResult loadBasicBinaryPayload(PC1600Machine& machine,
                                             const std::vector<uint8_t>& payload) {
    // The preset is responsible for the clean loadable state first -- PRO
    // mode (`- key: mode`) and a cleared program (`- type: NEW0`) -- the
    // same contract `format: basic-text` has. This loader types nothing.
    //
    // Placement is delegated to pc1600::planS0Placement(), which reads the
    // firmware-maintained work-area bytes (S0MTb / ADTBL / BASPRG_ST) and
    // reproduces where the real LOAD path would scatter the tokenised
    // image: internal RAM at $C0C5 on a stock machine, or across a fitted
    // module's 16 KB banks then internal RAM. The bytes are copied straight
    // into the emulator's backing store (debugWriteInternalRam /
    // debugWriteSlotImage), bypassing the bus and its post-NEW0 bank
    // gating.
    uint16_t stLh = readBE16(machine, kBasPrgSt);
    if (stLh == 0x0000 || stLh == 0xFFFF || stLh >= 0xF000) {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "BASPRG_ST looks uninitialised ($F865=$%04X) -- put the machine in PRO mode "
                      "and run NEW0 before the program block (`- key: mode` / `- type: NEW0`)",
                      stLh);
        return fail(buf);
    }

    pc1600::PlacementInput in;
    in.peek = [&machine](uint16_t a) { return machine.debugPeek(a); };
    in.slot1 = slotGeometry(machine, 1);
    in.slot2 = slotGeometry(machine, 2);

    pc1600::PlacementResult plan = pc1600::planS0Placement(in, payload.size());
    if (!plan.ok) return fail(plan.error);

    uint16_t f5cf = readLE16(machine, kRamBaseLE);
    if (f5cf != plan.startAddr) {
        // Not fatal (F5CF is a stack-region cell), but a mismatch means the
        // bank state is not what NEW0 usually leaves.
        std::fprintf(stderr, "[PC1600BasicLoader] note: $F5CF=$%04X != program start $%04X\n", f5cf,
                     plan.startAddr);
    }

    // The tokenised body followed by its single 0xFF terminator -- the
    // placement writes index into this buffer.
    std::vector<uint8_t> image = payload;
    image.push_back(0xFF);

    for (const pc1600::PlacementWrite& w : plan.writes) {
        const uint8_t* src = image.data() + w.sourceOffset;
        bool okWrite = (w.kind == pc1600::ProgramSegment::Kind::InternalRam)
                           ? machine.debugWriteInternalRam(w.backingOffset, src, w.length)
                           : machine.debugWriteSlotImage(w.slot, w.backingOffset, src, w.length);
        if (!okWrite) {
            char buf[192];
            if (w.kind == pc1600::ProgramSegment::Kind::InternalRam)
                std::snprintf(buf, sizeof(buf),
                              "backing-store write failed (internal RAM +$%X, %zu bytes)",
                              w.backingOffset, w.length);
            else
                std::snprintf(buf, sizeof(buf),
                              "backing-store write failed (slot %d image +$%X, %zu bytes)", w.slot,
                              w.backingOffset, w.length);
            return fail(buf);
        }
    }

    // BASPRG_END ($F867, big-endian) in the same LH5803-side representation
    // as BASPRG_ST: start value + program length (the 0xFF sits at that
    // address). The $F8xx work area is always a writable window, so the
    // pointer write goes through the ordinary bus path.
    uint32_t endLh = static_cast<uint32_t>(plan.basPrgStValue) +
                     static_cast<uint32_t>(payload.size());
    uint8_t endBytes[2] = {static_cast<uint8_t>((endLh >> 8) & 0xFF),
                           static_cast<uint8_t>(endLh & 0xFF)};
    if (!machine.pokeMemory(kBasPrgEnd, endBytes, 2)) {
        return fail("could not write BASPRG_END");
    }

    PC1600BasicLoadResult r;
    r.ok = true;
    r.baseAddr = plan.startAddr;
    r.endAddr = plan.endAddr;
    return r;
}
