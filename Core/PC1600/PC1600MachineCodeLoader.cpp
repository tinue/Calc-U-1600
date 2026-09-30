#include "PC1600MachineCodeLoader.hpp"

#include <cstdio>

#include "PC1600Machine.hpp"

bool loadPC1600MachineCode(PC1600Machine& machine, int slot, uint32_t addr, const uint8_t* data, size_t len,
                           std::string* error, int bank) {
    if (slot == 0) {
        if (addr < 0xC000 || static_cast<uint64_t>(addr) + len > 0x10000) {
            char b[160];
            std::snprintf(b, sizeof(b), "load $%04X + %zu bytes is outside the S0 internal-RAM window ($C000-$FFFF)",
                          addr, len);
            *error = b;
            return false;
        }
        if (!machine.debugWriteInternalRam(addr - 0xC000, data, len)) {
            *error = "backing-store write failed for slot S0 (empty slot, or a module with no writable RAM)";
            return false;
        }
        return true;
    }
    const char* slotName = (slot == 1) ? "S1" : "S2";
    if (addr < 0x8000 || static_cast<uint64_t>(addr) + len > 0xC000) {
        char b[192];
        std::snprintf(b, sizeof(b),
                      "load $%04X + %zu bytes does not fit the %s memory-slot window "
                      "($8000-$BFFF) -- use slot: S0 or split the image",
                      addr, len, slotName);
        *error = b;
        return false;
    }
    if (bank < 0) bank = slot == 1 ? 0 : 2;
    if (bank / 2 + 1 != slot) {
        char b[96];
        std::snprintf(b, sizeof(b), "bank %d is not in the %s memory slot", bank, slotName);
        *error = b;
        return false;
    }
    if (!machine.debugWriteSlotImage(slot, machinecode::pc1600ImageOffset(bank, addr), data, len)) {
        char b[160];
        std::snprintf(b, sizeof(b),
                      "backing-store write failed for slot %s, bank %d (empty slot, or a module with no writable "
                      "RAM there)",
                      slotName, bank);
        *error = b;
        return false;
    }
    return true;
}

pc1600::SlotGeometry pc1600SlotGeometry(PC1600Machine& machine, int slot) {
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

machinecode::PC1600State pc1600LoadState(PC1600Machine& machine) {
    machinecode::PC1600State st;
    st.mode1 = machine.mode1();
    st.title = machine.programAreaTitle();
    st.basicAreas = pc1600BasicAreas(machine);
    st.ramEnd = pc1600::lh5803ToZ80(static_cast<uint16_t>(machine.debugPeek(0xF864) << 8));
    if (st.title == 1 || st.title == 2) {
        const pc1600::SlotDescriptor d =
            pc1600::readSlotDescriptor([&machine](uint16_t a) { return machine.debugPeek(a); }, st.title);
        st.titleBase = static_cast<uint32_t>(d.basePage) << 8;
        st.titleStart = d.start;
    }
    for (int bank = 0; bank < 4; bank++) {
        for (uint32_t page = 0; page < 64; page++) {
            const uint32_t off = machinecode::pc1600ImageOffset(bank, 0x8000 + page * 0x100);
            if (machine.debugSlotImageWritable(bank / 2 + 1, off, 0x100))
                st.bankRamPages[static_cast<size_t>(bank)] |= uint64_t{1} << page;
        }
    }
    return st;
}

std::vector<machinecode::BasicArea> pc1600BasicAreas(PC1600Machine& machine) {
    pc1600::PlacementInput in;
    in.peek = [&machine](uint16_t a) { return machine.debugPeek(a); };
    in.slot1 = pc1600SlotGeometry(machine, 1);
    in.slot2 = pc1600SlotGeometry(machine, 2);
    const pc1600::PlacementResult plan = pc1600::planS0Placement(in, {});
    std::vector<machinecode::BasicArea> areas;
    if (!plan.ok) return areas;
    for (const pc1600::ProgramSegment& seg : plan.segments) {
        machinecode::BasicArea a;
        a.slot = seg.kind == pc1600::ProgramSegment::Kind::InternalRam ? 0 : seg.slot;
        a.windowBase = seg.windowBase;
        a.top = seg.top;
        // Backing offset of the window base (segment 0's base was moved up to BASPRG_ST).
        a.imageOffset = seg.backingBase - (static_cast<uint32_t>(seg.base) - seg.windowBase);
        areas.push_back(a);
    }
    return areas;
}
